/******************************************************************************
 * videofilter.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "videofilter.h"

#ifdef LDDECODE_HAVE_FFMPEG

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
}

#include <cstdio>

namespace {
// What ffmpeg said about the graph being built, which says more than the error code does
QString collectedLog;

void collectLog(void *context, int level, const char *format, va_list arguments)
{
    if (level > AV_LOG_ERROR) return;
    char line[512];
    vsnprintf(line, sizeof(line), format, arguments);
    (void)context;
    if (collectedLog.size() < 1000) collectedLog += QString::fromUtf8(line);
}

QString avError(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, text, sizeof(text));
    return QString::fromUtf8(text);
}

// An error message with the first thing ffmpeg logged about it
QString withLog(const QString &message)
{
    const QString first = collectedLog.section(QLatin1Char('\n'), 0, 0).trimmed();
    return first.isEmpty() ? message : message + " (" + first + ")";
}
}

bool VideoFilter::Format::operator==(const Format &other) const
{
    return width == other.width && height == other.height
           && displayWidth == other.displayWidth && displayHeight == other.displayHeight
           && frameRateNumerator == other.frameRateNumerator && frameRateDenominator == other.frameRateDenominator
           && topFieldFirst == other.topFieldFirst && interlaced == other.interlaced;
}

struct VideoFilter::Impl
{
    AVFilterGraph *graphContext = nullptr;
    AVFilterContext *source = nullptr;
    AVFilterContext *sink = nullptr;
    Format builtFor;
    QString builtGraph;
    int64_t nextPts = 0;
    bool built = false;

    ~Impl() { reset(); }

    void reset()
    {
        if (graphContext) avfilter_graph_free(&graphContext);
        graphContext = nullptr;
        source = sink = nullptr;
        built = false;
        nextPts = 0;
    }

    // Build the graph for an image format. Returns an error message, or an empty string.
    QString build(const QString &userGraph, const Format &format)
    {
        reset();

        // Keep what ffmpeg says about a mistake in the graph, instead of letting it print it
        collectedLog.clear();
        av_log_set_callback(collectLog);
        struct RestoreLog { ~RestoreLog() { av_log_set_callback(av_log_default_callback); } } restoreLog;

        QString description = userGraph.trimmed();
        description.replace("$W", QString::number(format.displayWidth));
        description.replace("$H", QString::number(format.displayHeight));
        // Whatever the graph does, the pixels come out as QImage::Format_RGB32 holds them
        description += ",format=pix_fmts=bgra";

        graphContext = avfilter_graph_alloc();
        if (!graphContext) return QStringLiteral("Out of memory");

        const QByteArray sourceArgs = QString("video_size=%1x%2:pix_fmt=%3:time_base=%4/%5:frame_rate=%5/%4:pixel_aspect=1/1")
                .arg(format.width).arg(format.height).arg(static_cast<int>(AV_PIX_FMT_BGRA))
                .arg(format.frameRateDenominator).arg(format.frameRateNumerator).toUtf8();
        int result = avfilter_graph_create_filter(&source, avfilter_get_by_name("buffer"), "in",
                                                  sourceArgs.constData(), nullptr, graphContext);
        if (result < 0) return avError(result);
        result = avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out",
                                              nullptr, nullptr, graphContext);
        if (result < 0) return avError(result);

        AVFilterInOut *outputs = avfilter_inout_alloc();
        AVFilterInOut *inputs = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            avfilter_inout_free(&outputs);
            avfilter_inout_free(&inputs);
            return QStringLiteral("Out of memory");
        }
        outputs->name = av_strdup("in");
        outputs->filter_ctx = source;
        outputs->pad_idx = 0;
        outputs->next = nullptr;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink;
        inputs->pad_idx = 0;
        inputs->next = nullptr;

        result = avfilter_graph_parse_ptr(graphContext, description.toUtf8().constData(), &inputs, &outputs, nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        if (result < 0) return withLog(avError(result));

        result = avfilter_graph_config(graphContext, nullptr);
        if (result < 0) return withLog(avError(result));

        builtFor = format;
        builtGraph = userGraph;
        built = true;
        nextPts = 0;
        return QString();
    }

    // Send one image in
    QString push(const QImage &image, const Format &format)
    {
        AVFrame *frame = av_frame_alloc();
        if (!frame) return QStringLiteral("Out of memory");
        frame->format = AV_PIX_FMT_BGRA;
        frame->width = image.width();
        frame->height = image.height();
        int result = av_frame_get_buffer(frame, 32);
        if (result < 0) {
            av_frame_free(&frame);
            return avError(result);
        }
        for (qint32 y = 0; y < image.height(); y++) {
            memcpy(frame->data[0] + static_cast<ptrdiff_t>(y) * frame->linesize[0], image.constScanLine(y),
                   static_cast<size_t>(image.width()) * 4);
        }
        frame->pts = nextPts++;
        if (format.interlaced) {
            // The picture is a frame of two interleaved fields
            frame->flags |= AV_FRAME_FLAG_INTERLACED;
            if (format.topFieldFirst) frame->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
        }

        result = av_buffersrc_add_frame_flags(source, frame, 0);
        av_frame_free(&frame);
        return result < 0 ? avError(result) : QString();
    }

    // Take an image out, if there is one
    QImage pull(QString &errorText)
    {
        AVFrame *frame = av_frame_alloc();
        if (!frame) return QImage();
        const int result = av_buffersink_get_frame(sink, frame);
        QImage output;
        if (result >= 0) {
            QImage copy(frame->width, frame->height, QImage::Format_RGB32);
            for (qint32 y = 0; y < frame->height; y++) {
                memcpy(copy.scanLine(y), frame->data[0] + static_cast<ptrdiff_t>(y) * frame->linesize[0],
                       static_cast<size_t>(frame->width) * 4);
            }
            output = copy;
        } else if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
            errorText = avError(result);
        }
        av_frame_free(&frame);
        return output;
    }
};

VideoFilter::VideoFilter() : impl(std::make_unique<Impl>()) {}
VideoFilter::~VideoFilter() = default;

bool VideoFilter::isAvailable() { return true; }

QString VideoFilter::setGraph(const QString &newGraph)
{
    graph = newGraph;
    error.clear();
    impl->reset();
    if (graph.trimmed().isEmpty()) return QString();

    // Try the graph on a small picture, to report a mistake in it at once
    Format probe;
    probe.width = 64;
    probe.height = 64;
    probe.displayWidth = 64;
    probe.displayHeight = 64;
    const QString problem = impl->build(graph, probe);
    impl->reset();
    if (!problem.isEmpty()) {
        error = problem;
        graph.clear();
    }
    return problem;
}

QImage VideoFilter::filter(const QImage &input, const Format &format, bool continuous)
{
    error.clear();
    if (!isActive() || input.isNull()) return input;

    QImage image = input;
    if (image.format() != QImage::Format_RGB32) image = image.convertToFormat(QImage::Format_RGB32);

    const bool rebuild = !impl->built || !(impl->builtFor == format) || impl->builtGraph != graph;
    if (rebuild || !continuous) {
        error = impl->build(graph, format);
        if (!error.isEmpty()) return QImage();
    }

    QImage result;
    if (continuous) {
        error = impl->push(image, format);
        if (!error.isEmpty()) return QImage();
        // The newest image out: filters that look ahead hold one back, so a frame
        // can come out later than it went in
        while (true) {
            QString pullError;
            QImage next = impl->pull(pullError);
            if (!pullError.isEmpty()) error = pullError;
            if (next.isNull()) break;
            result = next;
        }
    } else {
        // An image on its own: give it twice, so that filters that wait for the next
        // frame can produce this one, and take the first image out
        error = impl->push(image, format);
        if (error.isEmpty()) error = impl->push(image, format);
        if (!error.isEmpty()) return QImage();
        QString pullError;
        result = impl->pull(pullError);
        if (!pullError.isEmpty()) error = pullError;
        // The graph is of no use for what follows
        impl->reset();
    }
    return result;
}

#else   // no libavfilter in this build

struct VideoFilter::Impl {};

bool VideoFilter::Format::operator==(const Format &) const { return true; }
VideoFilter::VideoFilter() {}
VideoFilter::~VideoFilter() = default;
bool VideoFilter::isAvailable() { return false; }
QString VideoFilter::setGraph(const QString &) { return QStringLiteral("This build of ld-analyse has no video filters (libavfilter was not found)"); }
QImage VideoFilter::filter(const QImage &image, const Format &, bool) { return image; }

#endif
