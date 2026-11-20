/******************************************************************************
 * vlcsupport.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef VLCSUPPORT_H
#define VLCSUPPORT_H

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

// What the VLC that is installed offers: where it is, its video output modules and its deinterlacing modes, and the options of each module with their ranges.
// It is asked of VLC itself (vlc --full-help), so the modules that were added to it (an upscaler, a deinterlacer) are there as well, and no list is written down
// here. The question takes several seconds, so the answer is kept in a file (see load) and the caller asks on a thread.
namespace VlcSupport {

// The VLC to use: the one that was given, if it exists, then where the system puts it, then the PATH. Empty if it is not found.
QString find(const QString &configured);

// An option of a module that is a number or a switch, as VLC lists it
struct Option {
    QString name;            // as on the command line, without the dashes: "sharpen-sigma"
    QString description;
    bool isInteger = false;
    bool isBoolean = false;
    double minimum = 0.0, maximum = 1.0;
};

struct Help {
    QStringList videoOutputs;                      // the modules --vout takes (direct3d11, tscale...)
    QStringList deinterlaceModes;                  // what --deinterlace-mode takes (blend, yadif...)
    QMap<QString, QVector<Option>> modules;        // the options of each module that has some that are numbers or switches
    QMap<QString, QString> moduleNames;            // what VLC calls each module
};

// Ask VLC, or read what it said the first time it was asked about this file (it is asked again only with refresh, which is a button of the person's: it is for
// when a module was added to VLC). The question is a VLC that is run and waited for. False, with the reason, if it could not be run or said nothing that makes sense.
bool load(const QString &vlc, Help &help, QString &error, bool refresh = false);

// What is known of VLC without asking it: the usual video outputs and deinterlacers, and the modules of the usual picture adjustments (for when load fails)
void fallback(Help &help);

// The value that the control of an option starts at: VLC's own for the usual picture adjustments, the minimum for any other
double defaultOf(const Option &option);

// Parse the text of --full-help (for load, and the tests)
bool parse(const QString &text, Help &help);

}

#endif // VLCSUPPORT_H
