/******************************************************************************
 * remotelogic.h
 * ld-analyse - what the remote control decides: the number being typed and the speeds
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef REMOTELOGIC_H
#define REMOTELOGIC_H

#include <QString>
#include <QVector>

// What a search by number looks for
enum class SearchMode {
    Chapter,      // a chapter of the disc (the number of its VBI)
    Picture,      // a picture number of the disc (CAV)
    Time,         // a time of the disc (CLV), typed as hours, minutes and seconds
    Frame         // the frame of the capture (always there)
};

// The digits that are being typed with the keys of the remote, and what they mean in the mode of the search
class RemoteKeypad
{
public:
    void setMode(SearchMode newMode);
    SearchMode mode() const { return searchMode; }
    // The mode after this one among the chapter, the picture and the time, keeping to the ones the disc has (the chapter if it has none)
    static SearchMode nextMode(SearchMode current, bool chapters, bool pictures, bool times);

    void digit(qint32 value);     // 0 to 9; what does not fit in the number is not taken
    void plusTen();               // ten more (ten seconds, for a time)
    void clear();
    bool isEmpty() const { return digits.isEmpty(); }
    // The number: the chapter, the picture, the frame, or the seconds of the time; -1 if nothing was typed
    qint64 value() const;
    // The digits as one number whatever the mode is (the frame search takes them so); -1 if nothing was typed
    qint64 number() const;
    // What the display of the remote shows: the mode and the digits (a time as 0:12:30)
    QString text() const;
    static QString modeName(SearchMode mode);

private:
    SearchMode searchMode = SearchMode::Chapter;
    QString digits;
    qint32 capacity() const;
};

namespace RemoteSpeed {
    // The speeds of the multi-speed keys and of the shuttle, times the normal speed
    const QVector<double> &ladder();
    // The speed that follows (direction 1) or comes before (-1) a speed in the ladder; kept to its ends
    double step(double current, qint32 direction);
    // The speed that the shuttle ring has when it is turned from its centre by a part of its travel (-1 to 1, negative backward):
    // 0 near the centre, then the steps of the ladder, the sign being the way
    double fromDeflection(double deflection);
    // "x2", "x1/4": the speed as it is written on the display
    QString text(double speed);
}

// A time as hours, minutes and seconds
QString clockText(qint64 seconds);

#endif // REMOTELOGIC_H
