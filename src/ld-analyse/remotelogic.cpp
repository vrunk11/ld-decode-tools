/******************************************************************************
 * remotelogic.cpp
 * ld-analyse - what the remote control decides: the number being typed and the speeds
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "remotelogic.h"

#include <QtMath>

void RemoteKeypad::setMode(SearchMode newMode)
{
    if (newMode == searchMode) return;
    searchMode = newMode;
    digits.clear();
}

SearchMode RemoteKeypad::nextMode(SearchMode current, bool chapters, bool pictures, bool times)
{
    // (the frame has a key of its own, the frame search: the mode is the chapter, the picture or the time)
    const SearchMode order[] = { SearchMode::Chapter, SearchMode::Picture, SearchMode::Time };
    qint32 at = -1;
    for (qint32 i = 0; i < 3; i++) if (order[i] == current) at = i;
    for (qint32 count = 1; count <= 3; count++) {
        const SearchMode candidate = order[(at + count) % 3];
        if (candidate == SearchMode::Chapter && !chapters) continue;
        if (candidate == SearchMode::Picture && !pictures) continue;
        if (candidate == SearchMode::Time && !times) continue;
        return candidate;
    }
    return SearchMode::Chapter;
}

// How many digits may be typed: six for a time (hours, minutes, seconds), seven for the others (a frame number can have that many; a chapter or a
// picture that has more is not found)
qint32 RemoteKeypad::capacity() const
{
    return searchMode == SearchMode::Time ? 6 : 7;
}

void RemoteKeypad::digit(qint32 value)
{
    if (value < 0 || value > 9) return;
    // (a first zero is no use: it is not kept, except as the whole number)
    if (digits.isEmpty() && value == 0 && searchMode != SearchMode::Time) {
        digits = QStringLiteral("0");
        return;
    }
    if (digits == QLatin1String("0")) digits.clear();
    if (digits.size() >= capacity()) return;
    digits.append(QChar('0' + value));
}

qint64 RemoteKeypad::value() const
{
    if (digits.isEmpty()) return -1;
    const qint64 number = digits.toLongLong();
    if (searchMode != SearchMode::Time) return number;
    // The last two digits are seconds, the two before them minutes, the others hours
    const qint64 seconds = number % 100;
    const qint64 minutes = (number / 100) % 100;
    const qint64 hours = number / 10000;
    return hours * 3600 + minutes * 60 + seconds;
}

qint64 RemoteKeypad::number() const
{
    return digits.isEmpty() ? -1 : digits.toLongLong();
}

void RemoteKeypad::plusTen()
{
    const qint64 now = value();
    if (searchMode == SearchMode::Time) {
        const qint64 total = (now < 0 ? 0 : now) + 10;
        digits = QString::number((total / 3600) * 10000 + ((total / 60) % 60) * 100 + total % 60);
        return;
    }
    const qint64 total = (now < 0 ? 0 : now) + 10;
    digits = QString::number(total);
    if (digits.size() > capacity()) digits = digits.left(capacity());
}

void RemoteKeypad::clear()
{
    digits.clear();
}

QString RemoteKeypad::modeName(SearchMode mode)
{
    switch (mode) {
    case SearchMode::Chapter: return QStringLiteral("CHAPTER");
    case SearchMode::Picture: return QStringLiteral("PICTURE");
    case SearchMode::Time: return QStringLiteral("TIME");
    case SearchMode::Frame: return QStringLiteral("FRAME");
    }
    return QString();
}

QString RemoteKeypad::text() const
{
    if (searchMode == SearchMode::Time) {
        const qint64 seconds = value();
        return seconds < 0 ? QStringLiteral("-:--:--") : clockText(seconds);
    }
    return digits.isEmpty() ? QStringLiteral("_") : digits;
}

// The speeds ----------------------------------------------------------------------------------------------------------------

const QVector<double> &RemoteSpeed::ladder()
{
    static const QVector<double> speeds = { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0 };
    return speeds;
}

double RemoteSpeed::step(double current, qint32 direction)
{
    const QVector<double> &speeds = ladder();
    qint32 index = 0;
    // (the nearest speed of the ladder to the one there is)
    for (qint32 i = 1; i < speeds.size(); i++) {
        if (qAbs(speeds[i] - qAbs(current)) < qAbs(speeds[index] - qAbs(current))) index = i;
    }
    index = qBound(0, index + (direction > 0 ? 1 : -1), speeds.size() - 1);
    return speeds[index];
}

// The ring is still near its centre (a dead zone) and then goes through the speeds of the ladder in equal parts of the rest of its travel
double RemoteSpeed::fromDeflection(double deflection)
{
    static constexpr double DEAD_ZONE = 0.06;
    const double size = qAbs(deflection);
    if (size < DEAD_ZONE) return 0.0;
    const QVector<double> &speeds = ladder();
    const qint32 index = qMin(speeds.size() - 1, static_cast<qint32>((size - DEAD_ZONE) / (1.0 - DEAD_ZONE) * speeds.size()));
    return deflection < 0 ? -speeds[index] : speeds[index];
}

QString RemoteSpeed::text(double speed)
{
    const double size = qAbs(speed);
    QString number;
    if (size >= 1.0) number = QString::number(size, 'f', 0);
    else number = QStringLiteral("1/%1").arg(qRound(1.0 / size));
    return QStringLiteral("%1x%2").arg(speed < 0 ? QStringLiteral("-") : QString(), number);
}

QString clockText(qint64 seconds)
{
    return QStringLiteral("%1:%2:%3").arg(seconds / 3600).arg((seconds / 60) % 60, 2, 10, QChar('0')).arg(seconds % 60, 2, 10, QChar('0'));
}
