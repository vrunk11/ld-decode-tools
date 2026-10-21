/******************************************************************************
 * pipedinputoptions.h
 * ld-decode-tools - the options for video that comes in a pipe
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef PIPEDINPUTOPTIONS_H
#define PIPEDINPUTOPTIONS_H

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QString>

#include "cvbs.h"
#include "lddecodemetadata.h"

// The options of a tool that reads video from a pipe (ld-analyse, ld-chroma-decoder): what the pipe carries and, for video that comes with no metadata, the video
// system and its levels. Each is defined here once, with its name, its text and its check. A tool decides when they apply (what to do with them is its own),
// and has its own options for the buffer (--buffer-frames).
class PipedInputOptions
{
public:
    PipedInputOptions();

    // Register --input-format, --system, --black-level, --white-level, --subcarrier-locked and --sample-encoding with the parser
    void addTo(QCommandLineParser &parser);

    bool isFormatSet(const QCommandLineParser &parser) const { return parser.isSet(formatOption); }
    bool isSystemSet(const QCommandLineParser &parser) const { return parser.isSet(systemOption); }
    bool isLevelSet(const QCommandLineParser &parser) const { return parser.isSet(blackLevelOption) || parser.isSet(whiteLevelOption); }
    bool isSampleEncodingSet(const QCommandLineParser &parser) const { return parser.isSet(sampleEncodingOption); }
    bool isSubcarrierLockedSet(const QCommandLineParser &parser) const { return parser.isSet(subcarrierLockedOption); }

    // What the pipe carries, as --input-format says: "ldx", "tbc" or "cvbs" (empty if it was not given). False, having logged why, for another word.
    bool format(const QCommandLineParser &parser, QString &format) const;

    // The sample encoding of CVBS, as --sample-encoding says (u10, u16, tpg21, s16, or the full name). False, having logged why, for another word.
    bool sampleEncoding(const QCommandLineParser &parser, Cvbs::SampleEncoding &encoding) const;

    // The video system as --system says (pal, ntsc or pal-m). False, having logged why, for another word.
    bool videoSystem(const QCommandLineParser &parser, VideoSystem &system) const;

    // The black and white levels of TBC input, as --black-level and --white-level say (-1: not given). False, having logged why, if one is not above 0. The levels of
    // CVBS are the standard ones: if isCvbs, they are said to be ignored (and are -1).
    bool levels(const QCommandLineParser &parser, bool isCvbs, qint32 &blackLevel, qint32 &whiteLevel) const;

    // Whether PAL was sampled locked to the subcarrier, as --subcarrier-locked says (yes or no); defaultValue if it was not given. False, having logged why, for another word.
    bool subcarrierLocked(const QCommandLineParser &parser, bool defaultValue, bool &locked) const;

private:
    QCommandLineOption formatOption;
    QCommandLineOption systemOption;
    QCommandLineOption blackLevelOption;
    QCommandLineOption whiteLevelOption;
    QCommandLineOption subcarrierLockedOption;
    QCommandLineOption sampleEncodingOption;
};

#endif // PIPEDINPUTOPTIONS_H
