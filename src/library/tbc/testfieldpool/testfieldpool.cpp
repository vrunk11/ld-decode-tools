/******************************************************************************
 * testfieldpool.cpp
 * ld-decode-tools - unit tests for FieldPool
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCoreApplication>
#include <QFile>
#include <QMutex>
#include <QTemporaryDir>
#include <atomic>
#include <cstdlib>
#include <iostream>

#include "fieldpool.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

namespace {

std::atomic<int> workersMade{0};
std::atomic<qsizetype> lastDataSize{0};

// A worker that marks the VBI of every field it gets with the number of the field
class MarkingWorker : public QThread {
public:
    MarkingWorker(QAtomicInt &_abort, FieldPool &_pool) : abort(_abort), pool(_pool) {}

protected:
    void run() override {
        qint32 fieldNumber;
        SourceVideo::Data data;
        LdDecodeMetaData::Field field;
        LdDecodeMetaData::VideoParameters videoParameters;
        while (!abort && pool.getInputField(fieldNumber, data, field, videoParameters)) {
            lastDataSize = data.size();
            field.vbi.inUse = true;
            field.vbi.vbiData = { fieldNumber, 0, 0 };
            if (!pool.setOutputField(fieldNumber, field)) abort = 1;
        }
    }

private:
    QAtomicInt &abort;
    FieldPool &pool;
};

FieldPool::Settings settings(qint32 firstLine, qint32 lastLine)
{
    FieldPool::Settings pool;
    pool.name = "TEST";
    pool.firstLine = firstLine;
    pool.lastLine = lastLine;
    pool.createWorker = [](QAtomicInt &abort, FieldPool &fieldPool) -> QThread * { workersMade++; return new MarkingWorker(abort, fieldPool); };
    pool.storeField = [](LdDecodeMetaData &metadata, qint32 fieldNumber, const LdDecodeMetaData::Field &field) {
        metadata.updateFieldVbi(field.vbi, fieldNumber);
    };
    return pool;
}

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    std::cerr << "Testing FieldPool\n";

    const qint32 fields = 6;
    QTemporaryDir directory;
    CHECK(directory.isValid());

    // A capture of six fields of NTSC, with metadata that holds nothing yet
    LdDecodeMetaData metadata;
    metadata.initialiseDefault(NTSC);
    metadata.ensureFields(fields);
    metadata.setFormat(LdDecodeMetaData::MetadataFormat::Json);
    const LdDecodeMetaData::VideoParameters videoParameters = metadata.getVideoParameters();
    const qint32 width = videoParameters.fieldWidth;
    const qint32 height = videoParameters.fieldHeight;

    const QString tbc = directory.filePath("capture.tbc");
    {
        QFile file(tbc);
        CHECK(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(static_cast<qsizetype>(width) * height * 2 * fields, '\x10'));
    }

    // Every field goes to a worker once, what they found is in the metadata that is written, and the whole field is read
    const QString output = directory.filePath("capture.tbc.json");
    {
        FieldPool pool(settings(-1, -1), tbc, output, 3, metadata);
        CHECK(pool.process());
    }
    CHECK(workersMade == 3);
    CHECK(lastDataSize == static_cast<qsizetype>(width) * height);

    LdDecodeMetaData written;
    CHECK(written.read(output));
    CHECK(written.getNumberOfFields() == fields);
    for (qint32 field = 1; field <= fields; field++) {
        const LdDecodeMetaData::Vbi vbi = written.getFieldVbi(field);
        CHECK(vbi.inUse);
        CHECK(vbi.vbiData.size() == 3 && vbi.vbiData[0] == field);
    }

    // With lines, only those lines of the field are read (the last included)
    workersMade = 0;
    {
        FieldPool pool(settings(6, 22), tbc, directory.filePath("lines.tbc.json"), 2, metadata);
        CHECK(pool.process());
    }
    CHECK(workersMade == 2);
    CHECK(lastDataSize == static_cast<qsizetype>(width) * (22 - 6 + 1));

    // A capture that is not there fails, and the workers are not started
    workersMade = 0;
    {
        FieldPool pool(settings(-1, -1), directory.filePath("missing.tbc"), directory.filePath("none.json"), 2, metadata);
        CHECK(!pool.process());
    }
    CHECK(workersMade == 0);

    std::cerr << "All tests passed\n";
    return 0;
}
