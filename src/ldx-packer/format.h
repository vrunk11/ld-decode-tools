/******************************************************************************
 * format.h
 * ldx-packer - sizes, counts and durations as the window shows them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef LDXPACKER_FORMAT_H
#define LDXPACKER_FORMAT_H

#include <QString>

namespace fmt {

// 1.5 GB, 320 MB, 12 kB: decimal units, as the file managers of the systems show them
inline QString bytes(qint64 n)
{
    const char *units[] = { "B", "kB", "MB", "GB", "TB" };
    double value = static_cast<double>(n);
    int unit = 0;
    while (value >= 1000.0 && unit < 4) {
        value /= 1000.0;
        unit++;
    }
    return unit == 0 ? QStringLiteral("%1 B").arg(n) : QStringLiteral("%1 %2").arg(value, 0, 'f', value < 10.0 ? 2 : 1).arg(QLatin1String(units[unit]));
}

// 12 345 678, with thin spaces between the thousands
inline QString count(qint64 n)
{
    QString digits = QString::number(n);
    for (qsizetype i = digits.size() - 3; i > 0; i -= 3) digits.insert(i, QChar(0x202F));
    return digits;
}

// 1:02:03, 4:05, 12.3 s
inline QString duration(double seconds)
{
    if (seconds < 60.0) return QStringLiteral("%1 s").arg(seconds, 0, 'f', 1);
    const qint64 whole = static_cast<qint64>(seconds);
    const qint64 h = whole / 3600, m = (whole / 60) % 60, s = whole % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

} // namespace fmt

#endif // LDXPACKER_FORMAT_H
