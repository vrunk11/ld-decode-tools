/******************************************************************************
 * hvddecoder.cpp
 * ld-chroma-decoder - Colourisation filter for ld-decode
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 vrunk11
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "hvddecoder.h"

#include <algorithm>

#include "decoderpool.h"
#include "fieldwindow.h"
#include "lumacoring.h"

#include "deemp.h"
#include "firfilter.h"

#ifdef _OPENMP
#include <omp.h>
#endif

hvd::HvdConfig HvdSettings::makeConfig(double chromaGain, double chromaPhase) const
{
    hvd::HvdConfig config;
    config.chroma_gain = static_cast<float>(chromaGain);
    config.chroma_phase_deg = static_cast<float>(chromaPhase);
    config.cg_iterations = iterations;
    config.temporal_strength = static_cast<float>(strength);
    config.chunk_overlap = overlap;
    config.lambda_c = static_cast<float>(smoothness);
    config.fast = fast;
    config.field_order = fieldOrder;
    config.bidirectional = bidirectional;
    config.selective_3d = selective;
    config.passes = passes;
    config.acc = acc;
    config.monochrome = monochrome;
    config.symmetry_variant = symmetry;
    config.custom_subcarrier = customSubcarrier;
    config.subcarrier_hz = subcarrierHz;
    return config;
}

bool HvdFrameDecoder::makeGeometry(const LdDecodeMetaData::VideoParameters &videoParameters, hvd::FieldGeometry &g)
{
    g = hvd::FieldGeometry();
    switch (videoParameters.system) {
    case PAL: g.standard = hvd::VideoStandard::kPal; break;
    case PAL_M: g.standard = hvd::VideoStandard::kPalM; break;
    case NTSC: g.standard = hvd::VideoStandard::kNtsc; break;
    default:
        qCritical() << "The HVD decoder is for PAL, PAL-M and NTSC video sources only";
        return false;
    }
    g.field_width = videoParameters.fieldWidth;
    g.field_height = videoParameters.fieldHeight;
    g.active_video_start = videoParameters.activeVideoStart;
    g.active_video_end = videoParameters.activeVideoEnd;
    g.colour_burst_start = videoParameters.colourBurstStart;
    g.colour_burst_end = videoParameters.colourBurstEnd;
    // The active lines of a field, from those of the frame (a line of the frame is twice the line of the field, plus its parity)
    g.first_active_field_line = videoParameters.firstActiveFrameLine / 2;
    g.last_active_field_line = (videoParameters.lastActiveFrameLine + 1) / 2;
    g.sample_rate = videoParameters.sampleRate;
    if (!videoParameters.isSubcarrierLocked && videoParameters.fSC > 0.0) {
        g.subcarrier_hz = videoParameters.fSC;
    }

    if (g.last_active_field_line <= g.first_active_field_line || g.active_video_end <= g.active_video_start) {
        qCritical() << "The HVD decoder needs the active area of the picture in the metadata";
        return false;
    }
    if (videoParameters.blanking16bIre < 0 || videoParameters.white16bIre <= videoParameters.blanking16bIre) {
        qCritical() << "The HVD decoder needs the blanking and white levels in the metadata";
        return false;
    }
    return true;
}

bool HvdFrameDecoder::configure(const LdDecodeMetaData::VideoParameters &_videoParameters, const hvd::HvdConfig &config, bool _temporal)
{
    videoParameters = _videoParameters;
    temporal = _temporal;
    hvdConfig = config;
    hvdConfig.enable_temporal = temporal;
    // -1 is "off" for the temporal term; 0 would be "measure it"
    if (!temporal) hvdConfig.temporal_strength = -1.0F;
    if (!makeGeometry(videoParameters, geometry)) return false;
    if (hvdConfig.custom_subcarrier) geometry.subcarrier_hz = hvdConfig.subcarrier_hz;
    return true;
}

// The analogue-style noise reduction of the luma, as the other decoders do it (MonoDecoder::doYNR): what the high-pass of a line has above the level, is cut
void HvdFrameDecoder::reduceLumaNoise(ComponentFrame &frame) const
{
    if (lumaNR <= 0.0) return;

    const double irescale = (videoParameters.white16bIre - videoParameters.black16bIre) / 100.0;
    const double coring = lumaNR * irescale;
    const bool usePal = videoParameters.system == PAL || videoParameters.system == PAL_M;
    const auto &taps = usePal ? c_nrpal_b : c_nr_b;
    const auto &descriptor = usePal ? f_nrpal : f_nr;
    const qint32 delay = static_cast<qint32>(taps.size()) / 2;

    std::vector<double> hpY(static_cast<size_t>(videoParameters.activeVideoEnd + delay));
    for (qint32 line = videoParameters.firstActiveFrameLine; line < videoParameters.lastActiveFrameLine; line++) {
        auto yFilter(descriptor);  // a fresh filter for each line
        coreLumaLine(frame.y(line), videoParameters.activeVideoStart, videoParameters.activeVideoEnd, yFilter, delay, coring, hpY);
    }
}

qint32 HvdFrameDecoder::getLookBehind() const
{
    return temporal ? std::max(0, hvdConfig.chunk_overlap) : 0;
}

qint32 HvdFrameDecoder::getLookAhead() const
{
    return temporal ? std::max(0, hvdConfig.chunk_overlap) : 0;
}

void HvdFrameDecoder::decodeFrames(const QVector<SourceField> &inputFields, qint32 startIndex, qint32 endIndex,
                                   QVector<ComponentFrame> &componentFrames)
{
#ifdef _OPENMP
    if (threads > 0) omp_set_num_threads(threads);
#endif
    // The samples of the fields in IRE from blanking, the unit of the engine
    const double blanking = videoParameters.blanking16bIre;
    const double codesPerIre = (videoParameters.white16bIre - videoParameters.blanking16bIre) / 100.0;

    // The pool gives black fields beyond the ends of the capture. They are not context: the burst is not there to follow, and the
    // neighbours that the solver takes from them are wrong. Only the real fields around the frames are used.
    const quint16 black = static_cast<quint16>(videoParameters.black16bIre);
    const auto isBlank = [black](const SourceField &field) {
        return std::all_of(field.data.cbegin(), field.data.cend(), [black](quint16 sample) { return sample == black; });
    };
    qint32 first = startIndex;
    while (first >= 2 && !isBlank(inputFields[first - 2]) && !isBlank(inputFields[first - 1])) first -= 2;
    qint32 last = endIndex;
    while (last + 2 <= inputFields.size() && !isBlank(inputFields[last]) && !isBlank(inputFields[last + 1])) last += 2;

    std::vector<hvd::FieldInput> inputs(static_cast<size_t>(last - first));
    for (qint32 i = first; i < last; i++) {
        const SourceVideo::Data &data = inputFields[i].data;
        hvd::FieldInput &input = inputs[static_cast<size_t>(i - first)];
        input.samples = hvd::Plane(videoParameters.fieldHeight, videoParameters.fieldWidth);
        // The order of the fields: the metadata say which is the top one, unless the person does
        input.is_first_field = hvdConfig.field_order == 0 ? inputFields[i].field.isFirstField : ((i % 2 == 0) == (hvdConfig.field_order == 1));
        const qint64 count = std::min<qint64>(data.size(), static_cast<qint64>(videoParameters.fieldWidth) * videoParameters.fieldHeight);
        float *out = input.samples.data();
        for (qint64 s = 0; s < count; s++) out[s] = static_cast<float>((data[s] - blanking) / codesPerIre);
    }

    const hvd::FieldWindow window = hvd::DecodeFieldInputs(inputs, geometry, hvdConfig, engine);

    // Put the fields back into frames: a line of the frame comes from the field of its parity
    const qint32 firstLine = geometry.first_active_field_line;
    for (qint32 fieldIndex = startIndex, frameIndex = 0; fieldIndex < endIndex; fieldIndex += 2, frameIndex++) {
        ComponentFrame &frame = componentFrames[frameIndex];
        frame.init(videoParameters, hvdConfig.monochrome);

        // The field of each parity; if both claim one parity (a metadata fault) they keep the order they come in
        qint32 fieldOfParity[2] = { fieldIndex - first, fieldIndex + 1 - first };
        if (window.fields[fieldOfParity[0]].parity != window.fields[fieldOfParity[1]].parity && window.fields[fieldOfParity[0]].parity == 1) {
            std::swap(fieldOfParity[0], fieldOfParity[1]);
        }

        for (qint32 y = videoParameters.firstActiveFrameLine; y < videoParameters.lastActiveFrameLine; y++) {
            const qint32 fieldLine = y / 2 - firstLine;
            const hvd::DecodedField &decoded = window.decoded[static_cast<size_t>(fieldOfParity[y % 2])];
            if (fieldLine < 0 || fieldLine >= decoded.luma.height()) continue;

            double *outY = frame.y(y);
            double *outU = frame.u(y);
            double *outV = frame.v(y);
            const qint32 width = decoded.luma.width();
            for (qint32 c = 0; c < width; c++) {
                const qint32 x = videoParameters.activeVideoStart + c;
                outY[x] = blanking + decoded.luma.at(fieldLine, c) * codesPerIre;
                if (!hvdConfig.monochrome) {
                    const hvd::Complex chi = decoded.chroma.at(fieldLine, c);
                    outV[x] = window.gain * chi.real() * codesPerIre;
                    outU[x] = window.gain * -chi.imag() * codesPerIre;
                }
            }
        }
        reduceLumaNoise(frame);
    }
}

HvdDecoder::HvdDecoder(const hvd::HvdConfig &hvdConfig, bool temporal, double lumaNR)
{
    config.hvd = hvdConfig;
    config.temporal = temporal;
    config.lumaNR = lumaNR;
}

bool HvdDecoder::configure(const LdDecodeMetaData::VideoParameters &videoParameters)
{
    config.videoParameters = videoParameters;
    HvdFrameDecoder check;
    return check.configure(videoParameters, config.hvd, config.temporal);
}

qint32 HvdDecoder::getLookBehind() const
{
    return config.temporal ? std::max(0, config.hvd.chunk_overlap) : 0;
}

qint32 HvdDecoder::getLookAhead() const
{
    return config.temporal ? std::max(0, config.hvd.chunk_overlap) : 0;
}

QThread *HvdDecoder::makeThread(QAtomicInt &abort, DecoderPool &decoderPool)
{
    return new HvdThread(abort, decoderPool, config);
}

HvdThread::HvdThread(QAtomicInt &_abort, DecoderPool &_decoderPool,
                     const HvdDecoder::Configuration &config, QObject *parent)
    : DecoderThread(_abort, _decoderPool, parent)
{
    frameDecoder.configure(config.videoParameters, config.hvd, config.temporal);
    frameDecoder.setLumaNR(config.lumaNR);
}

void HvdThread::decodeFrames(const QVector<SourceField> &inputFields, qint32 startIndex, qint32 endIndex,
                             QVector<ComponentFrame> &componentFrames)
{
    // The pool already runs a thread per core: the engine stays on one in each
#ifdef _OPENMP
    omp_set_num_threads(1);
#endif
    frameDecoder.decodeFrames(inputFields, startIndex, endIndex, componentFrames);
}
