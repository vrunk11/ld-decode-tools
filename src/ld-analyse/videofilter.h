/******************************************************************************
 * videofilter.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef VIDEOFILTER_H
#define VIDEOFILTER_H

#include <QImage>
#include <QString>
#include <memory>

// Runs the image shown by ld-analyse through an ffmpeg filter graph (libavfilter), for
// deinterlacing, scaling and the like. The filters only change what is displayed: the
// analysis tools go on reading the TBC data.
//
// The graph is written as in ffmpeg's -vf option ("yadif", "bwdif,scale=...:flags=lanczos").
// $W and $H in it stand for the size the image will be shown at, so a scale filter can
// produce exactly that. Without libavfilter in the build, isAvailable() is false and
// nothing else works.
class VideoFilter
{
public:
    VideoFilter();
    ~VideoFilter();
    VideoFilter(const VideoFilter &) = delete;
    VideoFilter &operator=(const VideoFilter &) = delete;

    static bool isAvailable();

    struct Format {
        qint32 width = 0, height = 0;      // of the images given
        qint32 displayWidth = 0, displayHeight = 0;   // $W and $H
        qint32 frameRateNumerator = 25, frameRateDenominator = 1;
        bool topFieldFirst = true;
        bool interlaced = true;           // the picture is a frame of two fields
        bool operator==(const Format &other) const;
    };

    // Set the graph, returning an error message, or an empty string if it is good. The
    // graph is checked at once; it is built for the images when the first one comes.
    QString setGraph(const QString &graph);
    QString getGraph() const { return graph; }
    bool isActive() const { return !graph.trimmed().isEmpty(); }

    // Filter one image. If it follows the one before it (continuous), the graph keeps its
    // history, so filters that look at neighbouring frames work; the image that comes out
    // can then be an earlier one, or none at all (a null image), while the graph fills. For
    // an image on its own (after a seek, or paused) the graph is made afresh and the image
    // that comes out is always this one. On failure the result is null and lastError says why.
    QImage filter(const QImage &image, const Format &format, bool continuous);
    QString lastError() const { return error; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    QString graph;
    QString error;
};

#endif // VIDEOFILTER_H
