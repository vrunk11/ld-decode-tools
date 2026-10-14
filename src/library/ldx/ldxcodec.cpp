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
#include <libavutil/opt.h>
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

bool ffv1Available()
{
    return avcodec_find_encoder(AV_CODEC_ID_FFV1) && avcodec_find_decoder(AV_CODEC_ID_FFV1);
}

struct Ffv1Encoder::Private
{
    ContextPtr context;
    int width = 0;
    int height = 0;
    qint64 count = 0;
};

Ffv1Encoder::Ffv1Encoder() : d(new Private) {}
Ffv1Encoder::~Ffv1Encoder() = default;

bool Ffv1Encoder::open(int width, int height, QString &error)
{
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_FFV1);
    if (!codec) {
        error = QObject::tr("This build of ffmpeg has no FFV1 encoder");
        return false;
    }
    d->context.reset(avcodec_alloc_context3(codec));
    if (!d->context) {
        error = QObject::tr("Could not make an FFV1 encoder");
        return false;
    }
    AVCodecContext *context = d->context.get();
    context->width = width;
    context->height = height;
    context->pix_fmt = AV_PIX_FMT_GRAY10LE;
    context->time_base = AVRational { 1, 60 };
    context->framerate = AVRational { 60, 1 };
    context->gop_size = 1;                              // every frame stands alone
    context->thread_count = 1;
    context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;      // the decoder is given its configuration first
    av_opt_set_int(context->priv_data, "level", 3, 0);
    av_opt_set_int(context->priv_data, "coder", 1, 0);
    av_opt_set_int(context->priv_data, "context", 1, 0);
    av_opt_set_int(context->priv_data, "slices", 4, 0);
    av_opt_set_int(context->priv_data, "slicecrc", 1, 0);
    if (avcodec_open2(context, codec, nullptr) < 0) {
        error = QObject::tr("Could not open the FFV1 encoder");
        return false;
    }
    d->width = width;
    d->height = height;
    return true;
}

QByteArray Ffv1Encoder::extradata() const
{
    if (!d->context || d->context->extradata_size < 1) return QByteArray();
    return QByteArray(reinterpret_cast<const char *>(d->context->extradata), d->context->extradata_size);
}

bool Ffv1Encoder::encode(const QByteArray &image, QByteArray &packet, QString &error)
{
    packet.clear();
    if (!d->context || image.size() != d->width * d->height * 2) {
        error = QObject::tr("An FFV1 image is of %1 x %2 words").arg(d->width).arg(d->height);
        return false;
    }
    FramePtr frame(av_frame_alloc());
    frame->format = AV_PIX_FMT_GRAY10LE;
    frame->width = d->width;
    frame->height = d->height;
    if (av_frame_get_buffer(frame.get(), 0) < 0) {
        error = QObject::tr("Could not make a frame for the FFV1 encoder");
        return false;
    }
    for (int row = 0; row < d->height; row++) {
        std::memcpy(frame->data[0] + static_cast<qint64>(row) * frame->linesize[0], image.constData() + static_cast<qint64>(row) * d->width * 2,
                    static_cast<size_t>(d->width) * 2);
    }
    frame->pts = d->count++;
    frame->pict_type = AV_PICTURE_TYPE_I;

    PacketPtr out(av_packet_alloc());
    if (avcodec_send_frame(d->context.get(), frame.get()) < 0) {
        error = QObject::tr("The FFV1 encoder failed");
        return false;
    }
    const int result = avcodec_receive_packet(d->context.get(), out.get());
    if (result < 0) {
        error = QObject::tr("The FFV1 encoder gave nothing");
        return false;
    }
    packet = QByteArray(reinterpret_cast<const char *>(out->data), out->size);
    return true;
}

struct Ffv1Decoder::Private
{
    ContextPtr context;
    int width = 0;
    int height = 0;
};

Ffv1Decoder::Ffv1Decoder() : d(new Private) {}
Ffv1Decoder::~Ffv1Decoder() = default;

bool Ffv1Decoder::open(int width, int height, const QByteArray &extradata, QString &error)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_FFV1);
    if (!codec) {
        error = QObject::tr("This build of ffmpeg has no FFV1 decoder");
        return false;
    }
    d->context.reset(avcodec_alloc_context3(codec));
    if (!d->context) {
        error = QObject::tr("Could not make an FFV1 decoder");
        return false;
    }
    AVCodecContext *context = d->context.get();
    context->width = width;
    context->height = height;
    context->pix_fmt = AV_PIX_FMT_GRAY10LE;
    context->thread_count = 1;
    if (!extradata.isEmpty()) {
        context->extradata = static_cast<uint8_t *>(av_mallocz(static_cast<size_t>(extradata.size()) + AV_INPUT_BUFFER_PADDING_SIZE));
        std::memcpy(context->extradata, extradata.constData(), static_cast<size_t>(extradata.size()));
        context->extradata_size = extradata.size();
    }
    if (avcodec_open2(context, codec, nullptr) < 0) {
        error = QObject::tr("Could not open the FFV1 decoder");
        return false;
    }
    d->width = width;
    d->height = height;
    return true;
}

bool Ffv1Decoder::decode(const QByteArray &data, QByteArray &image, QString &error)
{
    image.clear();
    if (!d->context) {
        error = QObject::tr("The FFV1 decoder is not open");
        return false;
    }
    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    if (av_new_packet(packet.get(), data.size()) < 0) {
        error = QObject::tr("Out of memory");
        return false;
    }
    std::memcpy(packet->data, data.constData(), static_cast<size_t>(data.size()));
    if (avcodec_send_packet(d->context.get(), packet.get()) < 0 || avcodec_receive_frame(d->context.get(), frame.get()) < 0) {
        error = QObject::tr("The FFV1 decoder failed");
        return false;
    }
    if (frame->width != d->width || frame->height != d->height || frame->format != AV_PIX_FMT_GRAY10LE) {
        error = QObject::tr("The FFV1 image is not of %1 x %2 words of 10 bits").arg(d->width).arg(d->height);
        return false;
    }
    image.resize(d->width * d->height * 2);
    for (int row = 0; row < d->height; row++) {
        std::memcpy(image.data() + static_cast<qint64>(row) * d->width * 2, frame->data[0] + static_cast<qint64>(row) * frame->linesize[0],
                    static_cast<size_t>(d->width) * 2);
    }
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

bool ffv1Available() { return false; }

struct Ffv1Encoder::Private {};
Ffv1Encoder::Ffv1Encoder() : d(new Private) {}
Ffv1Encoder::~Ffv1Encoder() = default;
bool Ffv1Encoder::open(int, int, QString &error) { error = QObject::tr("This build has no FFV1: it was built without ffmpeg"); return false; }
QByteArray Ffv1Encoder::extradata() const { return QByteArray(); }
bool Ffv1Encoder::encode(const QByteArray &, QByteArray &, QString &error) { error = QObject::tr("This build has no FFV1: it was built without ffmpeg"); return false; }

struct Ffv1Decoder::Private {};
Ffv1Decoder::Ffv1Decoder() : d(new Private) {}
Ffv1Decoder::~Ffv1Decoder() = default;
bool Ffv1Decoder::open(int, int, const QByteArray &, QString &error) { error = QObject::tr("This build has no FFV1: it was built without ffmpeg"); return false; }
bool Ffv1Decoder::decode(const QByteArray &, QByteArray &, QString &error) { error = QObject::tr("This build has no FFV1: it was built without ffmpeg"); return false; }


#endif

QByteArray ffv1Pad(const QByteArray &samples, int width, int height, quint16 padValue)
{
    const qint64 words = static_cast<qint64>(width) * height;
    QByteArray image(static_cast<int>(words * 2), 0);
    const qint64 have = qMin<qint64>(samples.size() / 2, words);
    std::memcpy(image.data(), samples.constData(), static_cast<size_t>(have * 2));
    quint16 *target = reinterpret_cast<quint16 *>(image.data());
    for (qint64 i = have; i < words; i++) target[i] = padValue;
    return image;
}

QByteArray ffv1Unpad(const QByteArray &image, qint64 samples)
{
    return image.left(static_cast<int>(qMin<qint64>(samples * 2, image.size())));
}

} // namespace Ldx
