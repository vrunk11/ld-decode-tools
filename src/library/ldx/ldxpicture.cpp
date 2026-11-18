/******************************************************************************
 * ldxpicture.cpp
 * ld-decode-tools LDX library - the picture that is the cover of a file (JPEG, WebP, PNG, GIF, BMP), through ffmpeg
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "ldxpicture.h"

#include <QObject>
#include <algorithm>
#include <memory>
#include <cstring>

#ifdef LDX_WITH_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace Ldx {

QString pictureFormat(const QByteArray &data)
{
    const auto *p = reinterpret_cast<const unsigned char *>(data.constData());
    const int n = data.size();
    if (n >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') return QStringLiteral("png");
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return QStringLiteral("jpeg");
    if (n >= 12 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WEBP", 4) == 0) return QStringLiteral("webp");
    if (n >= 6 && std::memcmp(p, "GIF8", 4) == 0) return QStringLiteral("gif");
    if (n >= 26 && p[0] == 'B' && p[1] == 'M') return QStringLiteral("bmp");
    return QString();
}

Picture scaledToHeight(const Picture &picture, int maxHeight)
{
    if (picture.isNull() || picture.height <= maxHeight || maxHeight < 1) return picture;
    Picture scaled;
    scaled.height = maxHeight;
    scaled.width = std::max(1, static_cast<int>(static_cast<double>(picture.width) * maxHeight / picture.height + 0.5));
    scaled.rgba.resize(scaled.width * scaled.height * 4);
    const auto *source = reinterpret_cast<const unsigned char *>(picture.rgba.constData());
    auto *target = reinterpret_cast<unsigned char *>(scaled.rgba.data());
    // The average of the pixels that each pixel of the result covers (a box filter: it does not skip pixels when it makes a picture much smaller)
    for (int y = 0; y < scaled.height; ++y) {
        const int y0 = static_cast<int>(static_cast<qint64>(y) * picture.height / scaled.height);
        const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<qint64>(y + 1) * picture.height / scaled.height));
        for (int x = 0; x < scaled.width; ++x) {
            const int x0 = static_cast<int>(static_cast<qint64>(x) * picture.width / scaled.width);
            const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<qint64>(x + 1) * picture.width / scaled.width));
            qint64 sum[4] = { 0, 0, 0, 0 };
            int count = 0;
            for (int yy = y0; yy < y1 && yy < picture.height; ++yy) {
                const unsigned char *row = source + (static_cast<qint64>(yy) * picture.width + x0) * 4;
                for (int xx = x0; xx < x1 && xx < picture.width; ++xx, row += 4, ++count) {
                    for (int c = 0; c < 4; ++c) sum[c] += row[c];
                }
            }
            unsigned char *out = target + (static_cast<qint64>(y) * scaled.width + x) * 4;
            for (int c = 0; c < 4; ++c) out[c] = static_cast<unsigned char>(count > 0 ? (sum[c] + count / 2) / count : 0);
        }
    }
    return scaled;
}


// ---------------------------------------------------------------------------------------------------------------
// PNG, with the zlib of Qt Core (the ffmpeg of some builds has no PNG codec: it needs zlib)

namespace {

quint32 crc32Of(const QByteArray &type, const QByteArray &data)
{
    static quint32 table[256];
    static bool made = false;
    if (!made) {
        for (quint32 n = 0; n < 256; ++n) {
            quint32 c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        made = true;
    }
    quint32 crc = 0xFFFFFFFFu;
    for (char byte : type) crc = table[(crc ^ static_cast<unsigned char>(byte)) & 0xFF] ^ (crc >> 8);
    for (char byte : data) crc = table[(crc ^ static_cast<unsigned char>(byte)) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void putBigEndian(QByteArray &out, quint32 value)
{
    out.append(static_cast<char>(value >> 24));
    out.append(static_cast<char>(value >> 16));
    out.append(static_cast<char>(value >> 8));
    out.append(static_cast<char>(value));
}

void putChunk(QByteArray &out, const char *type, const QByteArray &data)
{
    putBigEndian(out, static_cast<quint32>(data.size()));
    out.append(type, 4);
    out.append(data);
    putBigEndian(out, crc32Of(QByteArray(type, 4), data));
}

bool decodePng(const QByteArray &data, Picture &picture, QString &error)
{
    const auto *p = reinterpret_cast<const unsigned char *>(data.constData());
    const qint64 n = data.size();
    const auto be = [&](qint64 at) { return (static_cast<quint32>(p[at]) << 24) | (static_cast<quint32>(p[at + 1]) << 16) | (static_cast<quint32>(p[at + 2]) << 8) | p[at + 3]; };
    quint32 w = 0, h = 0;
    int depth = 0, colour = 0, interlace = 0;
    QByteArray palette, alpha, compressed;
    qint64 at = 8;
    bool seenHeader = false;
    while (at + 12 <= n) {
        const quint32 length = be(at);
        const QByteArray type(data.constData() + at + 4, 4);
        if (at + 12 + static_cast<qint64>(length) > n) break;
        const QByteArray body = data.mid(static_cast<int>(at + 8), static_cast<int>(length));
        if (type == "IHDR" && length >= 13) {
            w = be(at + 8);
            h = be(at + 12);
            depth = p[at + 16];
            colour = p[at + 17];
            interlace = p[at + 20];
            seenHeader = true;
        } else if (type == "PLTE") {
            palette = body;
        } else if (type == "tRNS") {
            alpha = body;
        } else if (type == "IDAT") {
            compressed.append(body);
        } else if (type == "IEND") {
            break;
        }
        at += 12 + length;
    }
    if (!seenHeader || w == 0 || h == 0 || w > 20000 || h > 20000) {
        error = QObject::tr("The PNG picture is not valid");
        return false;
    }
    if (interlace != 0) {
        error = QObject::tr("An interlaced PNG picture is not read by this build");
        return false;
    }
    const int channels = colour == 0 ? 1 : (colour == 2 ? 3 : (colour == 3 ? 1 : (colour == 4 ? 2 : (colour == 6 ? 4 : 0))));
    if (channels == 0 || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) || (colour == 3 && depth == 16)) {
        error = QObject::tr("The format of this PNG picture is not read by this build");
        return false;
    }
    const qint64 bitsPerPixel = static_cast<qint64>(channels) * depth;
    const qint64 rowBytes = (static_cast<qint64>(w) * bitsPerPixel + 7) / 8;
    const qint64 rawSize = (rowBytes + 1) * h;
    // qUncompress wants the size of what is inside in front of the zlib stream
    QByteArray framed;
    putBigEndian(framed, static_cast<quint32>(rawSize));
    framed.append(compressed);
    const QByteArray raw = qUncompress(framed);
    if (raw.size() != rawSize) {
        error = QObject::tr("The PNG picture cannot be uncompressed");
        return false;
    }
    // Take the filters off, line by line
    const int bpp = static_cast<int>(std::max<qint64>(1, bitsPerPixel / 8));
    QByteArray pixels(static_cast<int>(rowBytes * h), 0);
    auto *out = reinterpret_cast<unsigned char *>(pixels.data());
    const auto *in = reinterpret_cast<const unsigned char *>(raw.constData());
    for (quint32 y = 0; y < h; ++y) {
        const int filter = in[y * (rowBytes + 1)];
        const unsigned char *line = in + y * (rowBytes + 1) + 1;
        unsigned char *row = out + y * rowBytes;
        const unsigned char *up = y > 0 ? out + (y - 1) * rowBytes : nullptr;
        for (qint64 i = 0; i < rowBytes; ++i) {
            const int a = i >= bpp ? row[i - bpp] : 0;
            const int b = up ? up[i] : 0;
            const int c = (up && i >= bpp) ? up[i - bpp] : 0;
            int predictor = 0;
            switch (filter) {
            case 1: predictor = a; break;
            case 2: predictor = b; break;
            case 3: predictor = (a + b) / 2; break;
            case 4: {
                const int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
                predictor = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default: break;
            }
            row[i] = static_cast<unsigned char>(line[i] + predictor);
        }
    }
    // To RGBA
    picture.width = static_cast<int>(w);
    picture.height = static_cast<int>(h);
    picture.rgba.resize(static_cast<int>(w * h * 4));
    auto *rgba = reinterpret_cast<unsigned char *>(picture.rgba.data());
    // The index-th sample of a line, as 8 bits (a 16-bit sample: its high byte; a palette index as it is)
    const auto sample = [&](const unsigned char *row, qint64 index) -> int {
        if (depth == 8) return row[index];
        if (depth == 16) return row[index * 2];
        const int perByte = 8 / depth;
        const int shift = 8 - depth * static_cast<int>(index % perByte + 1);
        const int value = (row[index / perByte] >> shift) & ((1 << depth) - 1);
        return colour == 3 ? value : value * 255 / ((1 << depth) - 1);
    };
    for (quint32 y = 0; y < h; ++y) {
        const unsigned char *row = out + y * rowBytes;
        for (quint32 x = 0; x < w; ++x) {
            unsigned char *o = rgba + (static_cast<qint64>(y) * w + x) * 4;
            switch (colour) {
            case 0:
                o[0] = o[1] = o[2] = static_cast<unsigned char>(sample(row, x));
                o[3] = 255;
                break;
            case 2:
                o[0] = static_cast<unsigned char>(sample(row, x * 3));
                o[1] = static_cast<unsigned char>(sample(row, x * 3 + 1));
                o[2] = static_cast<unsigned char>(sample(row, x * 3 + 2));
                o[3] = 255;
                break;
            case 3: {
                const int index = sample(row, x);
                const bool known = index * 3 + 2 < palette.size();
                o[0] = known ? static_cast<unsigned char>(palette[index * 3]) : 0;
                o[1] = known ? static_cast<unsigned char>(palette[index * 3 + 1]) : 0;
                o[2] = known ? static_cast<unsigned char>(palette[index * 3 + 2]) : 0;
                o[3] = index < alpha.size() ? static_cast<unsigned char>(alpha[index]) : 255;
                break;
            }
            case 4:
                o[0] = o[1] = o[2] = static_cast<unsigned char>(sample(row, x * 2));
                o[3] = static_cast<unsigned char>(sample(row, x * 2 + 1));
                break;
            default:
                o[0] = static_cast<unsigned char>(sample(row, x * 4));
                o[1] = static_cast<unsigned char>(sample(row, x * 4 + 1));
                o[2] = static_cast<unsigned char>(sample(row, x * 4 + 2));
                o[3] = static_cast<unsigned char>(sample(row, x * 4 + 3));
                break;
            }
        }
    }
    return true;
}

QByteArray encodePng(const Picture &picture)
{
    const qint64 w = picture.width;
    const qint64 h = picture.height;
    // Each line with the filter Sub (the difference to the pixel before), which compresses a picture a good deal better than none
    QByteArray raw;
    raw.resize(static_cast<int>((w * 4 + 1) * h));
    const auto *source = reinterpret_cast<const unsigned char *>(picture.rgba.constData());
    auto *out = reinterpret_cast<unsigned char *>(raw.data());
    for (qint64 y = 0; y < h; ++y) {
        unsigned char *line = out + y * (w * 4 + 1);
        line[0] = 1;
        const unsigned char *row = source + y * w * 4;
        for (qint64 i = 0; i < w * 4; ++i) line[1 + i] = static_cast<unsigned char>(row[i] - (i >= 4 ? row[i - 4] : 0));
    }
    QByteArray header;
    putBigEndian(header, static_cast<quint32>(w));
    putBigEndian(header, static_cast<quint32>(h));
    header.append(static_cast<char>(8));        // bits
    header.append(static_cast<char>(6));        // RGBA
    header.append(static_cast<char>(0));        // compression
    header.append(static_cast<char>(0));        // filter
    header.append(static_cast<char>(0));        // not interlaced
    QByteArray png("\x89PNG\r\n\x1a\n", 8);
    putChunk(png, "IHDR", header);
    putChunk(png, "IDAT", qCompress(raw, 6).mid(4));       // (qCompress puts the size in front of the zlib stream)
    putChunk(png, "IEND", QByteArray());
    return png;
}

}
#ifdef LDX_WITH_FFMPEG

namespace {

struct PacketDeleter { void operator()(AVPacket *packet) const { av_packet_free(&packet); } };
struct FrameDeleter { void operator()(AVFrame *frame) const { av_frame_free(&frame); } };
struct ContextDeleter { void operator()(AVCodecContext *context) const { avcodec_free_context(&context); } };
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using ContextPtr = std::unique_ptr<AVCodecContext, ContextDeleter>;

unsigned char clamp(double value)
{
    return static_cast<unsigned char>(value < 0.0 ? 0 : (value > 255.0 ? 255 : value + 0.5));
}

// A frame in the formats the decoders of those pictures give, as RGBA
bool frameToRgba(const AVFrame &frame, Picture &picture, QString &error)
{
    const int w = frame.width;
    const int h = frame.height;
    if (w < 1 || h < 1) {
        error = QObject::tr("The picture is empty");
        return false;
    }
    picture.width = w;
    picture.height = h;
    picture.rgba.resize(w * h * 4);
    auto *out = reinterpret_cast<unsigned char *>(picture.rgba.data());
    const auto format = static_cast<AVPixelFormat>(frame.format);

    const bool yuv = format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUV422P || format == AV_PIX_FMT_YUVJ422P
                     || format == AV_PIX_FMT_YUV444P || format == AV_PIX_FMT_YUVJ444P || format == AV_PIX_FMT_YUVA420P || format == AV_PIX_FMT_YUV440P
                     || format == AV_PIX_FMT_YUVJ440P;
    if (yuv) {
        const bool full = format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P || format == AV_PIX_FMT_YUVJ444P || format == AV_PIX_FMT_YUVJ440P
                          || frame.color_range == AVCOL_RANGE_JPEG;
        const int shiftX = (format == AV_PIX_FMT_YUV444P || format == AV_PIX_FMT_YUVJ444P || format == AV_PIX_FMT_YUV440P || format == AV_PIX_FMT_YUVJ440P) ? 0 : 1;
        const int shiftY = (format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVA420P || format == AV_PIX_FMT_YUV440P
                            || format == AV_PIX_FMT_YUVJ440P) ? 1 : 0;
        const bool alpha = format == AV_PIX_FMT_YUVA420P && frame.data[3];
        const double yScale = full ? 1.0 : 255.0 / 219.0;
        const double cScale = full ? 1.0 : 255.0 / 224.0;
        const double yBase = full ? 0.0 : 16.0;
        for (int y = 0; y < h; ++y) {
            const unsigned char *yp = frame.data[0] + static_cast<qint64>(y) * frame.linesize[0];
            const unsigned char *up = frame.data[1] + static_cast<qint64>(y >> shiftY) * frame.linesize[1];
            const unsigned char *vp = frame.data[2] + static_cast<qint64>(y >> shiftY) * frame.linesize[2];
            const unsigned char *ap = alpha ? frame.data[3] + static_cast<qint64>(y) * frame.linesize[3] : nullptr;
            for (int x = 0; x < w; ++x) {
                const double Y = (yp[x] - yBase) * yScale;
                const double U = (up[x >> shiftX] - 128.0) * cScale;
                const double V = (vp[x >> shiftX] - 128.0) * cScale;
                unsigned char *o = out + (static_cast<qint64>(y) * w + x) * 4;
                o[0] = clamp(Y + 1.402 * V);
                o[1] = clamp(Y - 0.344136 * U - 0.714136 * V);
                o[2] = clamp(Y + 1.772 * U);
                o[3] = ap ? ap[x] : 255;
            }
        }
        return true;
    }
    for (int y = 0; y < h; ++y) {
        const unsigned char *row = frame.data[0] + static_cast<qint64>(y) * frame.linesize[0];
        for (int x = 0; x < w; ++x) {
            unsigned char *o = out + (static_cast<qint64>(y) * w + x) * 4;
            switch (format) {
            case AV_PIX_FMT_RGB24: o[0] = row[x * 3]; o[1] = row[x * 3 + 1]; o[2] = row[x * 3 + 2]; o[3] = 255; break;
            case AV_PIX_FMT_BGR24: o[0] = row[x * 3 + 2]; o[1] = row[x * 3 + 1]; o[2] = row[x * 3]; o[3] = 255; break;
            case AV_PIX_FMT_RGBA: std::memcpy(o, row + x * 4, 4); break;
            case AV_PIX_FMT_BGRA: o[0] = row[x * 4 + 2]; o[1] = row[x * 4 + 1]; o[2] = row[x * 4]; o[3] = row[x * 4 + 3]; break;
            case AV_PIX_FMT_ARGB: o[0] = row[x * 4 + 1]; o[1] = row[x * 4 + 2]; o[2] = row[x * 4 + 3]; o[3] = row[x * 4]; break;
            case AV_PIX_FMT_ABGR: o[0] = row[x * 4 + 3]; o[1] = row[x * 4 + 2]; o[2] = row[x * 4 + 1]; o[3] = row[x * 4]; break;
            case AV_PIX_FMT_GRAY8: o[0] = o[1] = o[2] = row[x]; o[3] = 255; break;
            case AV_PIX_FMT_PAL8: {
                const auto *palette = reinterpret_cast<const quint32 *>(frame.data[1]);      // (0xAARRGGBB, in the order of the machine)
                const quint32 c = palette[row[x]];
                o[0] = (c >> 16) & 0xFF; o[1] = (c >> 8) & 0xFF; o[2] = c & 0xFF; o[3] = (c >> 24) & 0xFF;
                break;
            }
            case AV_PIX_FMT_RGB48LE: o[0] = row[x * 6 + 1]; o[1] = row[x * 6 + 3]; o[2] = row[x * 6 + 5]; o[3] = 255; break;
            case AV_PIX_FMT_RGBA64LE: o[0] = row[x * 8 + 1]; o[1] = row[x * 8 + 3]; o[2] = row[x * 8 + 5]; o[3] = row[x * 8 + 7]; break;
            case AV_PIX_FMT_GRAY16LE: o[0] = o[1] = o[2] = row[x * 2 + 1]; o[3] = 255; break;
            default:
                error = QObject::tr("The pixels of the picture are in a format this reader does not know");
                return false;
            }
        }
    }
    return true;
}

}

bool pictureAvailable()
{
    return avcodec_find_decoder(AV_CODEC_ID_MJPEG) && avcodec_find_encoder(AV_CODEC_ID_MJPEG);
}

bool decodePicture(const QByteArray &data, Picture &picture, QString &error)
{
    if (pictureFormat(data) == QLatin1String("png")) return decodePng(data, picture, error);
    const QString format = pictureFormat(data);
    AVCodecID id = AV_CODEC_ID_NONE;
    if (format == QLatin1String("jpeg")) id = AV_CODEC_ID_MJPEG;
    else if (format == QLatin1String("png")) id = AV_CODEC_ID_PNG;
    else if (format == QLatin1String("webp")) id = AV_CODEC_ID_WEBP;
    else if (format == QLatin1String("gif")) id = AV_CODEC_ID_GIF;
    else if (format == QLatin1String("bmp")) id = AV_CODEC_ID_BMP;
    if (id == AV_CODEC_ID_NONE) {
        error = QObject::tr("It is not a JPEG, WebP, PNG, GIF or BMP picture");
        return false;
    }
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) {
        error = QObject::tr("This build of ffmpeg cannot read %1 pictures").arg(format);
        return false;
    }
    ContextPtr context(avcodec_alloc_context3(codec));
    if (!context || avcodec_open2(context.get(), codec, nullptr) < 0) {
        error = QObject::tr("Could not open the decoder of %1 pictures").arg(format);
        return false;
    }
    PacketPtr packet(av_packet_alloc());
    if (av_new_packet(packet.get(), data.size()) < 0) {
        error = QObject::tr("Out of memory");
        return false;
    }
    std::memcpy(packet->data, data.constData(), static_cast<size_t>(data.size()));
    FramePtr frame(av_frame_alloc());
    if (avcodec_send_packet(context.get(), packet.get()) < 0) {
        error = QObject::tr("The %1 picture cannot be decoded").arg(format);
        return false;
    }
    avcodec_send_packet(context.get(), nullptr);        // (the end of the data: a decoder that holds the picture gives it)
    if (avcodec_receive_frame(context.get(), frame.get()) < 0) {
        error = QObject::tr("The %1 picture cannot be decoded").arg(format);
        return false;
    }
    return frameToRgba(*frame, picture, error);
}

bool encodePicture(const Picture &picture, bool jpeg, int quality, QByteArray &data, QString &error)
{
    if (picture.isNull()) {
        error = QObject::tr("There is no picture to write");
        return false;
    }
    if (!jpeg) {
        data = encodePng(picture);
        return true;
    }
    const AVCodec *codec = avcodec_find_encoder(jpeg ? AV_CODEC_ID_MJPEG : AV_CODEC_ID_PNG);
    if (!codec) {
        error = QObject::tr("This build of ffmpeg cannot write %1 pictures").arg(jpeg ? QStringLiteral("JPEG") : QStringLiteral("PNG"));
        return false;
    }
    ContextPtr context(avcodec_alloc_context3(codec));
    if (!context) {
        error = QObject::tr("Could not make an encoder");
        return false;
    }
    const int w = picture.width;
    const int h = picture.height;
    context->width = w;
    context->height = h;
    context->time_base = AVRational { 1, 25 };
    context->pix_fmt = jpeg ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_RGBA;
    if (jpeg) {
        context->color_range = AVCOL_RANGE_JPEG;
        context->flags |= AV_CODEC_FLAG_QSCALE;
        const int qscale = std::max(1, std::min(31, 2 + (100 - std::max(1, std::min(100, quality))) / 8));
        context->global_quality = FF_QP2LAMBDA * qscale;
    }
    if (avcodec_open2(context.get(), codec, nullptr) < 0) {
        error = QObject::tr("Could not open the encoder");
        return false;
    }
    FramePtr frame(av_frame_alloc());
    frame->format = context->pix_fmt;
    frame->width = w;
    frame->height = h;
    if (jpeg) {
        frame->quality = context->global_quality;
        frame->color_range = AVCOL_RANGE_JPEG;
    }
    if (av_frame_get_buffer(frame.get(), 0) < 0) {
        error = QObject::tr("Out of memory");
        return false;
    }
    const auto *rgba = reinterpret_cast<const unsigned char *>(picture.rgba.constData());
    if (!jpeg) {
        for (int y = 0; y < h; ++y) std::memcpy(frame->data[0] + static_cast<qint64>(y) * frame->linesize[0], rgba + static_cast<qint64>(y) * w * 4, static_cast<size_t>(w) * 4);
    } else {
        // RGB to full-range YUV (BT.601), the colour of 2 x 2 pixels shared; the transparent parts are white
        const auto at = [&](int x, int y, double &r, double &g, double &b) {
            const unsigned char *p = rgba + (static_cast<qint64>(std::min(y, h - 1)) * w + std::min(x, w - 1)) * 4;
            const double a = p[3] / 255.0;
            r = p[0] * a + 255.0 * (1.0 - a);
            g = p[1] * a + 255.0 * (1.0 - a);
            b = p[2] * a + 255.0 * (1.0 - a);
        };
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                double r, g, b;
                at(x, y, r, g, b);
                frame->data[0][static_cast<qint64>(y) * frame->linesize[0] + x] = clamp(0.299 * r + 0.587 * g + 0.114 * b);
            }
        }
        for (int y = 0; y < (h + 1) / 2; ++y) {
            for (int x = 0; x < (w + 1) / 2; ++x) {
                double ur = 0, ug = 0, ub = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        double r, g, b;
                        at(x * 2 + dx, y * 2 + dy, r, g, b);
                        ur += r; ug += g; ub += b;
                    }
                }
                ur /= 4; ug /= 4; ub /= 4;
                const double Y = 0.299 * ur + 0.587 * ug + 0.114 * ub;
                frame->data[1][static_cast<qint64>(y) * frame->linesize[1] + x] = clamp(128.0 + 0.564 * (ub - Y));
                frame->data[2][static_cast<qint64>(y) * frame->linesize[2] + x] = clamp(128.0 + 0.713 * (ur - Y));
            }
        }
    }
    PacketPtr packet(av_packet_alloc());
    if (avcodec_send_frame(context.get(), frame.get()) < 0 || avcodec_send_frame(context.get(), nullptr) < 0 || avcodec_receive_packet(context.get(), packet.get()) < 0) {
        error = QObject::tr("The picture cannot be encoded");
        return false;
    }
    data = QByteArray(reinterpret_cast<const char *>(packet->data), packet->size);
    return true;
}

#else // no ffmpeg

bool pictureAvailable() { return false; }

bool decodePicture(const QByteArray &data, Picture &picture, QString &error)
{
    if (pictureFormat(data) == QLatin1String("png")) return decodePng(data, picture, error);
    error = QObject::tr("This build has no ffmpeg to read this format of picture with (a PNG is read without it)");
    return false;
}

bool encodePicture(const Picture &picture, bool jpeg, int, QByteArray &data, QString &error)
{
    if (!jpeg && !picture.isNull()) {
        data = encodePng(picture);
        return true;
    }
    error = QObject::tr("This build has no ffmpeg to write JPEG pictures with");
    return false;
}

#endif

}
