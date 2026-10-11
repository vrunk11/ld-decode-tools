/******************************************************************************
 * targetvideo.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef TARGETVIDEO_H
#define TARGETVIDEO_H

#include <QFile>
#include <QString>
#include <QVector>
#include <memory>

#include "cvbs.h"

// Writes video fields: as they are to a TBC file (or stdout), or, for a .cvbs,
// .cvbsy or .cvbsc file, joined in pairs into CVBS frames. Fields must come in
// frame order (first field, then second) for CVBS.
class TargetVideo
{
public:
    TargetVideo() = default;
    ~TargetVideo();

    TargetVideo(const TargetVideo &) = delete;
    TargetVideo &operator=(const TargetVideo &) = delete;

    // Open the output; "-" is stdout. The video system and sample encoding
    // matter only for CVBS output.
    bool open(const QString &fileName, VideoSystem system,
              Cvbs::SampleEncoding encoding = Cvbs::SampleEncoding::U10_4FSC);
    bool writeField(const QVector<quint16> &fieldData);
    // CVBS only: correct the levels of the fields written from now on
    // (see Cvbs::SampleConverter::setLevelMap)
    void setLevelMap(double scale, double offset);
    // Returns false if the output ends with half a CVBS frame (which is dropped)
    bool close();

    bool isCvbs() const { return cvbs; }

private:
    QFile file;
    bool cvbs = false;
    Cvbs::Geometry geometry;
    std::unique_ptr<Cvbs::SampleConverter> converter;
    QVector<quint16> pendingField;
    bool hasPendingField = false;
    QVector<qint16> frameData;
};

// Processing tools keep their output in the input's container (TBC or CVBS),
// as they keep the metadata format: ld-tbc-to-cvbs and ld-cvbs-to-tbc convert.
// Returns false, having logged why, if outputFileName would be a conversion.
bool checkSameVideoContainer(const QString &inputFileName, const QString &outputFileName);

#endif // TARGETVIDEO_H
