/******************************************************************************
 * stackproject.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef STACKPROJECT_H
#define STACKPROJECT_H

#include <QString>
#include <QVector>

// One source of the stack
struct StackSource {
    QString id;            // names the source in the session; kept when the sources are reordered
    QString file;          // absolute path in memory, relative to the session file in the file
    QString label;
    bool enabled = true;   // an off source stays in the list but is left out of the stack
    qint32 offset = 0;     // the source is shifted by this many frames to align it with the others
};

// How the sources are stacked (the options of ld-disc-stacker)
struct StackSettings {
    qint32 mode = -1;      // -1 auto, 0 mean, 1 median, 2 smart mean, 3 smart neighbour, 4 neighbour
    qint32 smartThreshold = 15;
    bool diffDod = true;
    bool passthrough = false;
    bool integrity = false;
    bool reverseFields = false;
};

// A stacking session: what the .stack file holds. The GUI saves and loads it, so that the work can
// be taken up again; the output block is what a command line stacking needs.
class StackProject
{
public:
    static constexpr qint32 CURRENT_VERSION = 1;

    QVector<StackSource> sources;
    StackSettings stacking;
    QString outputFile;                    // absolute in memory
    QString metadataFormat = "db";         // db or json

    // What only the GUI uses
    bool chroma = false;
    double zoom = 1.0;
    bool aspect = true;                    // shown at the display aspect ratio (4:3 or 16:9), not as square pixels
    bool fieldView = false;                // one field of the frame, not the frame
    bool fieldSplit = false;               // in field view: both fields, one above the other, not one field with its lines doubled
    QString selected;                      // the id of the source whose tab is chosen
    bool showStack = true;                 // the picture is the stack, or (off) the source chosen

    // Read a session file. Returns false, with the reason in error, when it cannot be used.
    bool load(const QString &fileName, QString &error);
    // Write a session file (paths relative to it)
    bool save(const QString &fileName, QString &error) const;

    // An id that no source has yet
    QString newSourceId() const;
    // The source with this id, or -1
    qint32 indexOfSource(const QString &id) const;
};

#endif // STACKPROJECT_H
