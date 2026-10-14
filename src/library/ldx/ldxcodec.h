/******************************************************************************
 * ldxcodec.h
 * ld-decode-tools LDX library - the codecs of the tracks (FLAC, through ffmpeg)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef LDXCODEC_H
#define LDXCODEC_H

#include <QByteArray>
#include <QString>
#include <memory>
#include <vector>

// FLAC for the blocks of 16-bit samples of a track. ffmpeg (libavcodec) encodes and decodes; the LDX library puts what it
// gives in the container (a block of the file holds the FLAC frames of one block of samples, laced).
// Without ffmpeg none of this is available, and the library says so for a track that needs it.
namespace Ldx {

// Whether this build has FLAC
bool flacAvailable();

// The FLAC frames of one block of samples are of this many samples, so a block of up to 8 of them is
// 262,144 samples (a field of a TBC is 239,330)
constexpr int FlacFrameSamples = 32768;
constexpr int FlacMaxFrames = 8;

// The sample rate and compression level FLAC is given. The rate is not the rate of the samples (FLAC limits it): the
// real one is in the parameters of the track.
constexpr int FlacStandInRate = 48000;
constexpr int FlacCompressionLevel = 8;

// What a FLAC decoder is given before the data: "fLaC" and the STREAMINFO block, as in a Matroska track
QByteArray flacCodecPrivate(int channels, QString &error);

// A block of samples (little-endian 16-bit, the channels one after the other for each sample) as FLAC frames. `unsignedSamples` says the samples are unsigned (TBC), which
// are made signed by taking 32768 from them, so that the wave does not jump where it crosses the middle.
bool flacEncode(const QByteArray &samples, int channels, bool unsignedSamples, std::vector<QByteArray> &frames, QString &error);

class FlacDecoder
{
public:
    FlacDecoder();
    ~FlacDecoder();
    // `codecPrivate` is what flacCodecPrivate gave
    bool open(const QByteArray &codecPrivate, QString &error);
    bool decode(const std::vector<QByteArray> &frames, bool unsignedSamples, QByteArray &samples, QString &error);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

// FFV1 for the frames of a track of CVBS samples (10 bits, in 16-bit words). A frame is an image as wide as a line, with the rows
// that fit; when the last row is not whole it is filled up (a PAL frame has 709,379 samples: 625 lines and 4 samples, so 626 rows).
bool ffv1Available();

// The rows of the image of a frame of `samples` samples in lines of `width`
inline int ffv1Height(qint64 samples, int width)
{
    return static_cast<int>((samples + width - 1) / width);
}

// A frame (little-endian 16-bit words) as the words of an image of width * height, the end filled with padValue
QByteArray ffv1Pad(const QByteArray &samples, int width, int height, quint16 padValue);
// And back: the first `samples` words of an image
QByteArray ffv1Unpad(const QByteArray &image, qint64 samples);

class Ffv1Encoder
{
public:
    Ffv1Encoder();
    ~Ffv1Encoder();
    bool open(int width, int height, QString &error);
    QByteArray extradata() const;                 // the decoder is given it first (the CodecPrivate of the track)
    // An image of width * height words as one packet, every one a key frame
    bool encode(const QByteArray &image, QByteArray &packet, QString &error);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

class Ffv1Decoder
{
public:
    Ffv1Decoder();
    ~Ffv1Decoder();
    bool open(int width, int height, const QByteArray &extradata, QString &error);
    bool decode(const QByteArray &packet, QByteArray &image, QString &error);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace Ldx

#endif // LDXCODEC_H
