/******************************************************************************
 * targetvideo.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "targetvideo.h"

#include <QDebug>
#include <cstdio>

TargetVideo::~TargetVideo()
{
    if (file.isOpen()) file.close();
}

bool TargetVideo::open(const QString &fileName, VideoSystem system, Cvbs::SampleEncoding encoding)
{
    cvbs = fileName != "-" && Cvbs::isDataFileName(fileName);
    hasPendingField = false;

    if (cvbs) {
        geometry = Cvbs::geometry(system);
        converter = std::make_unique<Cvbs::SampleConverter>(encoding, system);
        frameData.resize(static_cast<qint32>(geometry.frameSamples));
    }

    if (fileName == "-") {
        if (!file.open(stdout, QIODevice::WriteOnly)) {
            qCritical() << "Unable to open stdout for the output video";
            return false;
        }
    } else {
        file.setFileName(fileName);
        if (!file.open(QIODevice::WriteOnly)) {
            qCritical().noquote() << "Unable to open output video file" << fileName;
            return false;
        }
    }

    return true;
}

bool TargetVideo::writeField(const QVector<quint16> &fieldData)
{
    if (!cvbs) {
        const qint64 bytes = 2 * static_cast<qint64>(fieldData.size());
        return file.write(reinterpret_cast<const char *>(fieldData.constData()), bytes) == bytes;
    }

    if (fieldData.size() != geometry.fieldLength()) {
        qCritical() << "Field of" << fieldData.size() << "samples does not fit a CVBS frame of"
                    << geometry.fieldLength() << "sample fields";
        return false;
    }

    // Hold the first field of each frame until the second arrives
    if (!hasPendingField) {
        pendingField = fieldData;
        hasPendingField = true;
        return true;
    }

    Cvbs::fieldsToFrame(pendingField.constData(), fieldData.constData(), geometry, *converter, frameData.data());
    hasPendingField = false;

    const qint64 bytes = 2 * static_cast<qint64>(frameData.size());
    return file.write(reinterpret_cast<const char *>(frameData.constData()), bytes) == bytes;
}

void TargetVideo::setLevelMap(double scale, double offset)
{
    if (converter) converter->setLevelMap(scale, offset);
}

bool TargetVideo::close()
{
    if (file.isOpen()) file.close();

    if (hasPendingField) {
        qWarning() << "The output ended with an unpaired field, which is not written: CVBS files hold whole frames";
        hasPendingField = false;
        return false;
    }
    return true;
}

bool checkSameVideoContainer(const QString &inputFileName, const QString &outputFileName)
{
    // stdout carries whatever the input is
    if (outputFileName == "-") return true;

    const bool inputIsCvbs = Cvbs::isDataFileName(inputFileName);
    const bool outputIsCvbs = Cvbs::isDataFileName(outputFileName);
    if (inputIsCvbs == outputIsCvbs) return true;

    if (inputIsCvbs) {
        qCritical().noquote() << "The input is CVBS but" << outputFileName
                              << "is not a .cvbs file - use ld-cvbs-to-tbc to convert to TBC";
    } else {
        qCritical().noquote() << "The input is TBC but" << outputFileName
                              << "is a CVBS file - use ld-tbc-to-cvbs to convert to CVBS";
    }
    return false;
}
