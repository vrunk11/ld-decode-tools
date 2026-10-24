/******************************************************************************
 * hvddecoder.h
 * ld-chroma-decoder - Colourisation filter for ld-decode
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 vrunk11
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef HVDDECODER_H
#define HVDDECODER_H

#include <QAtomicInt>
#include <QThread>

#include "decoder.h"
#include "engine/engine.h"
#include "engine/hvd_config.h"
#include "engine/ntsc_geometry.h"

class DecoderPool;

// What a person chooses for the HVD decoder (the holographic-variational Y/C separator of lddecode-hvd), in the command line and in ld-analyse
struct HvdSettings {
    bool enabled = false;       // the HVD decoder is the one that is used
    bool temporal = false;      // 3D: the fields of the frames around a frame are used too
    qint32 iterations = 2;      // conjugate-gradient iterations of the solver
    double strength = 0.0;      // weight of the neighbouring fields; 0 is measured on the picture
    qint32 overlap = 1;         // frames on each side of a frame that 3D looks at
    bool fast = true;           // the cheaper solver: early exit and a smaller iteration budget, at about the same quality
    qint32 fieldOrder = 0;      // 0 = from the metadata, 1 = the first field of a frame is the top one, 2 = the bottom one
    bool bidirectional = true;  // 3D: the fields after a field are used as well as the ones before
    bool selective = false;     // 3D only on the tiles of the picture where luma and chroma are ambiguous
    qint32 passes = 2;          // 3D: passes of the solver over a window (from the second the noise-reduced anchor is used)
    bool acc = true;            // automatic colour control: saturation brought to the nominal burst
    bool monochrome = false;    // the chroma is zeroed in the output
    double smoothness = 1.0;    // chroma smoothness: higher is smoother chroma, lower is sharper
    bool symmetry = false;      // the spectral-symmetry initialisation as well
    bool customSubcarrier = false;      // the colour subcarrier is not the one of the standard (the VHD, for one)
    double subcarrierHz = 2556818.2;    // its frequency: the VHD, the exact line lock 162.5 x fH

    // The engine's configuration for these settings, with the chroma gain and phase that every decoder shares
    hvd::HvdConfig makeConfig(double chromaGain, double chromaPhase) const;

    // The name of the decoder in ld-chroma-decoder --decoder and in the decoding parameters: hvd2d or hvd3d
    QString decoderName() const { return temporal ? QStringLiteral("hvd3d") : QStringLiteral("hvd2d"); }
};

// Decodes the frames of a run of fields with the HVD decoder; one is for one thread (it keeps the plans of the FFTs)
class HvdFrameDecoder {
public:
    // The geometry of the engine for a video, or false (with a message) if the decoder cannot work on it
    static bool makeGeometry(const LdDecodeMetaData::VideoParameters &videoParameters, hvd::FieldGeometry &geometry);

    // Set up for a video; false (with a message) if the decoder cannot work on it
    bool configure(const LdDecodeMetaData::VideoParameters &videoParameters, const hvd::HvdConfig &config, bool temporal);

    // Frames the decoder needs to see before and after the ones it decodes (each frame is two SourceFields)
    qint32 getLookBehind() const;
    qint32 getLookAhead() const;

    // Threads (OpenMP) that the decoder may use in the thread that calls decodeFrames: 0 leaves them as they are
    void setThreads(qint32 count) { threads = count; }

    // The level of the noise reduction of the luma, in IRE (0 is none)
    void setLumaNR(double level) { lumaNR = level; }

    // Decode the frames from startIndex to endIndex of inputFields (the fields before and after are the context)
    void decodeFrames(const QVector<SourceField> &inputFields, qint32 startIndex, qint32 endIndex,
                      QVector<ComponentFrame> &componentFrames);

private:
    LdDecodeMetaData::VideoParameters videoParameters;
    hvd::HvdConfig hvdConfig;
    hvd::FieldGeometry geometry;
    bool temporal = false;
    qint32 threads = 0;
    double lumaNR = 0.0;

    void reduceLumaNoise(ComponentFrame &frame) const;
    hvd::HvdEngine engine;
};

// The decoder of ld-chroma-decoder: PAL, PAL-M and NTSC, in 2D (every field by itself) or 3D (the frames around it are looked at too)
class HvdDecoder : public Decoder {
public:
    // Parameters used by HvdDecoder and HvdThread
    struct Configuration : public Decoder::Configuration {
        hvd::HvdConfig hvd;
        bool temporal = false;
        double lumaNR = 0.0;
    };

    HvdDecoder(const hvd::HvdConfig &hvdConfig, bool temporal, double lumaNR = 0.0);
    bool configure(const LdDecodeMetaData::VideoParameters &videoParameters) override;
    qint32 getLookBehind() const override;
    qint32 getLookAhead() const override;
    QThread *makeThread(QAtomicInt &abort, DecoderPool &decoderPool) override;

private:
    Configuration config;
};

class HvdThread : public DecoderThread
{
    Q_OBJECT
public:
    explicit HvdThread(QAtomicInt &abort, DecoderPool &decoderPool,
                       const HvdDecoder::Configuration &config, QObject *parent = nullptr);

protected:
    void decodeFrames(const QVector<SourceField> &inputFields, qint32 startIndex, qint32 endIndex,
                      QVector<ComponentFrame> &componentFrames) override;

private:
    HvdFrameDecoder frameDecoder;
};

#endif // HVDDECODER_H
