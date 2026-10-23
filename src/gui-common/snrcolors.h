/******************************************************************************
 * snrcolors.h
 * ld-decode-tools GUI common - the colours of the SNR graphs
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef SNRCOLORS_H
#define SNRCOLORS_H

#include <QColor>

// The white SNR and the black PSNR are drawn in these colours in every window that graphs them (ld-analyse and ld-stacker-gui), so that a curve is the same
// curve wherever it is seen
inline QColor snrWhiteColor() { return QColor("#378ADD"); }
inline QColor snrBlackColor() { return QColor("#E8833A"); }

#endif // SNRCOLORS_H
