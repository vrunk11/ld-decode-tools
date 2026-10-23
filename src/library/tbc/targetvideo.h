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
#include <QJsonArray>
#include <QJsonObject>
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

    // An output that is an LDX file (name.ldx): the video is a TBC track of it, with a track for its metadata (which the
    // tool writes afterwards to the same name, see LdDecodeMetaData::write) and, if asked, one for the analogue audio. Say
    // what the video is, and the history of the capture with what the tool did (see LdxCapture::historyOf and historyEntry), before open().
    void setLdxVideo(qint32 fieldWidth, qint32 fieldHeight, double sampleRate, const QString &name, const QJsonArray &history);
    // The output "-" is an LDX stream instead of a raw TBC: it begins with the parameters of the video (ld-decode metadata
    // with no fields, see LdDecodeMetaData::parametersJson) and then each field comes with its own metadata, as a block
    // of JSON just before its video (writeField with the JSON). Before open().
    void setLdxStream(const QByteArray &parametersJson);
    // The video is CVBS: the track of an LDX output is a cvbs track, whose blocks are frames (two fields each, joined as
    // in a .cvbs file; the samples are kept as 10-bit values, see the specification). Before open().
    void setLdxCvbs(bool isCvbs) { ldxCvbs = isCvbs; }
    // Whether the video a tool reads (a file, a track of an LDX file or the LDX stream "-") is CVBS
    static bool sourceIsCvbs(const QString &inputName);
    bool writeField(const QVector<quint16> &fieldData, const QByteArray &metadataJson);
    // bits: 16 for the analogue audio of a TBC capture, 24 for that of a CVBS capture
    void setLdxAudio(double sampleRate, qint32 bits = 16);
    // 16-bit stereo audio of the fields written so far, for an LDX output (anything else: false, the tool writes its own file)
    bool writeAudio(const QByteArray &samples);
    bool isLdx() const;

    // Another stream of the output LDX file, copied from a source (see StreamCopy): its track is declared before open() and its blocks are written with the time they
    // have in the source. kind and codec are the names of the specification (efm, ac3, rf, pcm; raw, flac). Returns the number to write to, or -1 (not an LDX output).
    qint32 addLdxTrack(const QString &kind, const QString &codec, qint32 depth, const QString &name, const QJsonObject &parameters);
    bool writeLdxTrack(qint32 index, qint64 timeNs, const QByteArray &data);
    // The time of the next field of the output, in nanoseconds: the blocks of the other streams that come before it can be written
    qint64 ldxTimeNs() const;

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

    struct LdxOutput;

private:
    QFile file;
    bool cvbs = false;
    Cvbs::Geometry geometry;
    std::unique_ptr<Cvbs::SampleConverter> converter;
    QVector<quint16> pendingField;
    bool hasPendingField = false;
    QVector<qint16> frameData;
    std::shared_ptr<LdxOutput> ldx;
    QJsonArray ldxHistory;
    bool ldxStreamOutput = false;
    bool ldxCvbs = false;
    QByteArray ldxParameters;
    quint32 ldxMetadataTrack = 0;
    qint32 ldxFieldWidth = 0, ldxFieldHeight = 0;
    double ldxSampleRate = 0;
    QString ldxName;
    struct ExtraTrack { QString kind, codec, name; qint32 depth = 0; QJsonObject parameters; quint32 number = 0; };
    QVector<ExtraTrack> ldxExtraTracks;
    double ldxAudioRate = 0;
    qint32 ldxAudioBits = 16;
    VideoSystem ldxSystem = PAL;
};

// Processing tools keep their output in the input's container (TBC or CVBS),
// as they keep the metadata format: ld-tbc-to-cvbs and ld-cvbs-to-tbc convert.
// Returns false, having logged why, if outputFileName would be a conversion.
bool checkSameVideoContainer(const QString &inputFileName, const QString &outputFileName);

#endif // TARGETVIDEO_H
