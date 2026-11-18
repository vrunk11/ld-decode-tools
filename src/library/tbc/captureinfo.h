/******************************************************************************
 * captureinfo.h
 * ld-decode-tools - what a person says of a capture: its medium, its reference, its face (section 3.1 and 8.1 of the LDX specification)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef CAPTUREINFO_H
#define CAPTUREINFO_H

#include <QString>
#include <QStringList>
#include <QVector>

// The words a person's description of a capture is made of: the same ones as the .rfx archives of RF-tools use, so that what an archive says is copied as it is.
// Each parse gives the value as a file writes it (lower case, but for the face) and false when the text is none of them; the empty text (and "unknown", for the
// medium) is "not said" and gives the empty value.
namespace CaptureInfo {

// What was captured: a LaserDisc, a VHS tape, a VideoDisc, a composite signal from something else
const QStringList &media();
bool parseMedium(const QString &text, QString &value);
// The name a person reads: "LD", "VHS", "VHD", "CVBS" or "unknown"
QString mediumName(const QString &medium);

// What the tools show of a capture, as rows (a name and a value) for what is said and what is known; nothing for what is not said. ldxName is an LDX file and its track
// ("file.ldx#2").
struct Row {
    QString name;
    QString value;
};
QVector<Row> saidRows(const QString &ldxName);
// Where the capture comes from (the track ldx.rfsource, section 8.4)
QVector<Row> rfSourceRows(const QString &ldxName);


// The face of the disc: A, B, AB or BA
const QStringList &faces();
bool parseFace(const QString &text, QString &value);

}

#endif // CAPTUREINFO_H
