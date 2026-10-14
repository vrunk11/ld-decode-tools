/******************************************************************************
 * ldxcodec.cpp
 * ld-decode-tools LDX library - the codecs of the tracks (FLAC, through ffmpeg)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "ldxcodec.h"

#include <QObject>
#include <cstring>

#ifdef LDX_WITH_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}
#endif

namespace Ldx {

#ifdef LDX_WITH_FFMPEG

namespace {

struct PacketDeleter { void operator()(AVPacket *packet) const { av_packet_free(&packet); } };
struct FrameDeleter { void operator()(AVFrame *frame) const { av_frame_free(&frame); } };
struct ContextDeleter { void operator()(AVCodecContext *context) const { avcodec_free_context(&context); } };

using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using ContextPtr = std::unique_ptr<AVCodecContext, ContextDeleter>;

// An encoder for one block: it is made for each, because it takes a frame that is shorter than the others only as the last
ContextPtr openEncoder(int channels, QString &error)
{
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
    if (!codec) {
        error = QObject::tr("This build of ffmpeg has no FLAC encoder");
        return nullptr;
    }
    ContextPtr context(avcodec_alloc_context3(codec));
    if (!context) {
        error = QObject::tr("Could not make a FLAC encoder");
        return nullptr;
    }
    context->sample_fmt = AV_SAMPLE_FMT_S16;
    context->sample_rate = FlacStandInRate;
    av_channel_layout_default(&context->ch_layout, channels);
    context->compression_level = FlacCompressionLevel;
    context->frame_size = FlacFrameSamples;
    if (avcodec_open2(context.get(), codec, nullptr) < 0) {
        error = QObject::tr("Could not open the FLAC encoder");
        return nullptr;
    }
    return context;
}

} // namespace

bool flacAvailable()
{
    return avcodec_find_encoder(AV_CODEC_ID_FLAC) && avcodec_find_decoder(AV_CODEC_ID_FLAC);
}

QByteArray flacCodecPrivate(int channels, QString &error)
{
    ContextPtr context = openEncoder(channels, error);
    if (!context) return QByteArray();
    if (context->extradata_size < 34) {
        error = QObject::tr("The FLAC encoder gave no STREAMINFO");
        return QByteArray();
    }
    // "fLaC", then one metadata block, the last (0x80) and of type 0 (STREAMINFO), 34 bytes long
    QByteArray header("fLaC", 4);
    header.append(static_cast<char>(0x80));
    header.append(static_cast<char>(0));
    header.append(static_cast<char>(0));
    header.append(static_cast<char>(34));
    header.append(reinterpret_cast<const char *>(context->extradata), 34);
    return header;
}

bool flacEncode(const QByteArray &samples, int channels, bool unsignedSamples, std::vector<QByteArray> &frames, QString &error)
{
    frames.clear();
    const int count = samples.size() / (2 * channels);        // per channel
    if (count < 1 || samples.size() % (2 * channels) != 0) {
        error = QObject::tr("A block of FLAC samples is of 16-bit samples, and not empty");
        return false;
    }
    if (count > FlacFrameSamples * FlacMaxFrames) {
        error = QObject::tr("A block of %1 samples is too long for FLAC (%2 at most)").arg(count).arg(FlacFrameSamples * FlacMaxFrames);
        return false;
    }
    ContextPtr context = openEncoder(channels, error);
    if (!context) return false;
    PacketPtr packet(av_packet_alloc());

    const auto drain = [&]() {
        for (;;) {
            const int result = avcodec_receive_packet(context.get(), packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
            if (result < 0) return false;
            // (the flush gives an empty packet, which carries only the side data of the end)
            if (packet->size > 0) frames.emplace_back(reinterpret_cast<const char *>(packet->data), packet->size);
            av_packet_unref(packet.get());
        }
    };

    const quint16 *source = reinterpret_cast<const quint16 *>(samples.constData());
    for (int done = 0; done < count; done += FlacFrameSamples) {
        const int n = qMin(FlacFrameSamples, count - done);
        FramePtr frame(av_frame_alloc());
        frame->nb_samples = n;
        frame->format = AV_SAMPLE_FMT_S16;
        frame->sample_rate = FlacStandInRate;
        av_channel_layout_copy(&frame->ch_layout, &context->ch_layout);
        if (av_frame_get_buffer(frame.get(), 0) < 0) {
            error = QObject::tr("Could not make a frame for the FLAC encoder");
            return false;
        }
        qint16 *target = reinterpret_cast<qint16 *>(frame->data[0]);
        for (int i = 0; i < n * channels; i++) {
            const quint16 value = source[done * channels + i];
            target[i] = static_cast<qint16>(unsignedSamples ? static_cast<quint16>(value ^ 0x8000u) : value);
        }
        if (avcodec_send_frame(context.get(), frame.get()) < 0 || !drain()) {
            error = QObject::tr("The FLAC encoder failed");
            return false;
        }
    }
    if (avcodec_send_frame(context.get(), nullptr) < 0 || !drain()) {
        error = QObject::tr("The FLAC encoder failed at the end of a block");
        return false;
    }
    if (frames.empty()) {
        error = QObject::tr("The FLAC encoder gave nothing");
        return false;
    }
    return true;
}

struct FlacDecoder::Private
{
    ContextPtr context;
    QByteArray extradata;
};

FlacDecoder::FlacDecoder() : d(new Private) {}
FlacDecoder::~FlacDecoder() = default;

bool FlacDecoder::open(const QByteArray &codecPrivate, QString &error)
{
    // The STREAMINFO block is after "fLaC" and the 4 bytes that say what the block is and how long
    if (codecPrivate.size() < 8 + 34 || codecPrivate.left(4) != "fLaC") {
        error = QObject::tr("The FLAC track has no STREAMINFO");
        return false;
    }
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_FLAC);
    if (!codec) {
        error = QObject::tr("This build of ffmpeg has no FLAC decoder");
        return false;
    }
    d->context.reset(avcodec_alloc_context3(codec));
    if (!d->context) {
        error = QObject::tr("Could not make a FLAC decoder");
        return false;
    }
    d->context->extradata = static_cast<uint8_t *>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(d->context->extradata, codecPrivate.constData() + 8, 34);
    d->context->extradata_size = 34;
    if (avcodec_open2(d->context.get(), codec, nullptr) < 0) {
        error = QObject::tr("Could not open the FLAC decoder");
        return false;
    }
    return true;
}

bool FlacDecoder::decode(const std::vector<QByteArray> &frames, bool unsignedSamples, QByteArray &samples, QString &error)
{
    samples.clear();
    if (!d->context) {
        error = QObject::tr("The FLAC decoder is not open");
        return false;
    }
    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());

    const auto receive = [&]() {
        for (;;) {
            const int result = avcodec_receive_frame(d->context.get(), frame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
            if (result < 0) return false;
            const int n = frame->nb_samples;
            const int channels = frame->ch_layout.nb_channels;
            const int old = samples.size();
            samples.resize(old + n * channels * 2);
            quint16 *target = reinterpret_cast<quint16 *>(samples.data() + old);
            const bool planar = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format)) != 0;
            for (int i = 0; i < n; i++) {
                for (int channel = 0; channel < channels; channel++) {
                    const qint16 *source = reinterpret_cast<const qint16 *>(planar ? frame->data[channel] : frame->data[0]);
                    const quint16 value = static_cast<quint16>(planar ? source[i] : source[i * channels + channel]);
                    target[i * channels + channel] = unsignedSamples ? static_cast<quint16>(value ^ 0x8000u) : value;
                }
            }
            av_frame_unref(frame.get());
        }
    };

    for (const QByteArray &data: frames) {
        // (the packet holds a copy: the decoder may keep it)
        if (av_new_packet(packet.get(), data.size()) < 0) {
            error = QObject::tr("Out of memory");
            return false;
        }
        std::memcpy(packet->data, data.constData(), static_cast<size_t>(data.size()));
        const int sent = avcodec_send_packet(d->context.get(), packet.get());
        av_packet_unref(packet.get());
        if (sent < 0 || !receive()) {
            char text[128] = "";
            av_strerror(sent, text, sizeof(text));
            error = QObject::tr("The FLAC decoder failed (%1, frame of %2 bytes)").arg(QString::fromLatin1(text)).arg(data.size());
            return false;
        }
    }
    // Nothing is kept by a FLAC decoder from one block to the next
    avcodec_send_packet(d->context.get(), nullptr);
    if (!receive()) {
        error = QObject::tr("The FLAC decoder failed at the end of a block");
        return false;
    }
    avcodec_flush_buffers(d->context.get());
    return true;
}

#else // no ffmpeg

bool flacAvailable()
{
    return false;
}

QByteArray flacCodecPrivate(int, QString &error)
{
    error = QObject::tr("This build has no FLAC: it was built without ffmpeg");
    return QByteArray();
}

bool flacEncode(const QByteArray &, int, bool, std::vector<QByteArray> &, QString &error)
{
    error = QObject::tr("This build has no FLAC: it was built without ffmpeg");
    return false;
}

struct FlacDecoder::Private {};
FlacDecoder::FlacDecoder() : d(new Private) {}
FlacDecoder::~FlacDecoder() = default;

bool FlacDecoder::open(const QByteArray &, QString &error)
{
    error = QObject::tr("This build has no FLAC: it was built without ffmpeg");
    return false;
}

bool FlacDecoder::decode(const std::vector<QByteArray> &, bool, QByteArray &, QString &error)
{
    error = QObject::tr("This build has no FLAC: it was built without ffmpeg");
    return false;
}

#endif

} // namespace Ldx
