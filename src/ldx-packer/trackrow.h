/******************************************************************************
 * trackrow.h
 * ldx-packer - a line of the list of captures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef TRACKROW_H
#define TRACKROW_H

#include "document.h"

#include <QColor>
#include <QWidget>

// A line as in the list of ld-stacker-gui: a bar of the colour of the capture on the left (wider when it is the one picked), a
// faint tint of that colour across, the number, the name with what is known of it, how it stands (kept, new, recode, removed),
// and the buttons to move it and to take it out.
class TrackRow : public QWidget
{
    Q_OBJECT

public:
    // expandable: it has tracks that go with it to show under it (expanded: they are shown)
    TrackRow(const Entry &entry, int index, EntryState state, const QColor &color, bool picked, bool canUp, bool canDown, bool expandable, bool expanded, QWidget *parent = nullptr);

signals:
    void picked(int index);
    void moved(int index, int delta);
    void removeAsked(int index);
    void restoreAsked(int index);
    void expandToggled(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    int m_index;
    QColor m_color;
    bool m_picked;
    bool m_faded;
};


// A track that goes with a capture, under its line in the list: what it is (the kind), its name and what it holds, as a line of its own that is picked
// to be seen and changed in the panel. Indented, with a fainter tint of the colour of the capture.
class TrackChildRow : public QWidget
{
    Q_OBJECT

public:
    // capture: the index of the capture it goes with, row: its place in the tracks that go with it (-1: the notes)
    TrackChildRow(const Companion &track, int capture, int row, const QColor &color, bool picked, QWidget *parent = nullptr);
    // The notes of a capture
    TrackChildRow(const QString &notes, int capture, const QColor &color, bool picked, QWidget *parent = nullptr);

signals:
    void picked(int capture, int row);
    void removeAsked(int capture, int row);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    void build(const QString &kind, const QString &text, const QString &details, bool faded);

    int m_capture;
    int m_row;
    QColor m_color;
    bool m_picked;
    bool m_faded = false;
};
#endif // TRACKROW_H
