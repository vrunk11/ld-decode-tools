/******************************************************************************
 * testsqliteschema.cpp
 * ld-decode-tools - the SQLite schema the library creates is the one ld-decode creates
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCoreApplication>
#include <QMap>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>

#include "lddecodemetadata.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

// The text that makes each table of a database (sqlite_master), with the spaces made alike
static QMap<QString, QString> tablesOf(const QString &fileName, const QString &connection)
{
    QMap<QString, QString> tables;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(fileName);
        CHECK(db.open());
        QSqlQuery query(db);
        CHECK(query.exec("SELECT name, sql FROM sqlite_master WHERE type = 'table' AND sql IS NOT NULL"));
        while (query.next()) {
            QString sql = query.value(1).toString();
            sql.replace(QRegularExpression("\\s+"), " ").replace("( ", "(").replace(" )", ")");
            tables.insert(query.value(0).toString(), sql);
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return tables;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    // The path of a .tbc.db file that ld-decode made (test-data), to compare with
    CHECK(argc == 2);

    std::cerr << "Testing that a new .tbc.db has the schema that ld-decode makes\n";

    // A file with something in every table
    QTemporaryDir directory;
    const QString fileName = directory.filePath("new.tbc.db");
    {
        LdDecodeMetaData metadata;
        metadata.initialiseDefault(NTSC);
        LdDecodeMetaData::PcmAudioParameters audio;
        audio.isValid = true;
        audio.bits = 16;
        audio.sampleRate = 44100;
        metadata.setPcmAudioParameters(audio);
        metadata.ensureFields(2);
        LdDecodeMetaData::Field field = metadata.getField(1);
        field.vbi.inUse = true;
        field.vbi.vbiData = { 1, 2, 3 };
        field.vitc.inUse = true;
        field.closedCaption.inUse = true;
        field.vitsMetrics.inUse = true;
        field.vitsMetrics.wSNR = 38.0;
        field.vitsMetrics.bPSNR = 40.0;
        // the same dropout twice
        field.dropOuts.append(10, 20, 5);
        field.dropOuts.append(10, 20, 5);
        field.dropOuts.append(30, 35, 5);
        metadata.updateField(field, 1);
        metadata.setFormat(LdDecodeMetaData::MetadataFormat::Sqlite);
        CHECK(metadata.write(fileName));
    }

    const QMap<QString, QString> got = tablesOf(fileName, "new");
    const QMap<QString, QString> expected = tablesOf(QString::fromLocal8Bit(argv[1]), "ld-decode");
    CHECK(expected.size() == 8);
    CHECK(got.keys() == expected.keys());
    for (const QString &name: expected.keys()) {
        if (got.value(name) != expected.value(name)) {
            std::cerr << "table " << name.toStdString() << " is not as ld-decode makes it:\n  " << got.value(name).toStdString() << "\n  " << expected.value(name).toStdString() << "\n";
            return 1;
        }
    }

    std::cerr << "Testing that a dropout that is written twice is kept once\n";
    LdDecodeMetaData back;
    CHECK(back.read(fileName));
    const DropOuts &dropOuts = back.getFieldDropOuts(1);
    CHECK(dropOuts.size() == 2);
    // (the order of the dropouts of a field that is read from SQLite is not the one they were written in: it is not checked here)
    bool have10 = false, have30 = false;
    for (qint32 i = 0; i < dropOuts.size(); i++) {
        if (dropOuts.startx(i) == 10 && dropOuts.endx(i) == 20 && dropOuts.fieldLine(i) == 5) have10 = true;
        if (dropOuts.startx(i) == 30 && dropOuts.endx(i) == 35 && dropOuts.fieldLine(i) == 5) have30 = true;
    }
    CHECK(have10 && have30);

    std::cerr << "All tests passed\n";
    return 0;
}
