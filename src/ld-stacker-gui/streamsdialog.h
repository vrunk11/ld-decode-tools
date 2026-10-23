/******************************************************************************
 * streamsdialog.h
 * ld-stacker-gui - which other streams of the sources go into the LDX file of the stack
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef STREAMSDIALOG_H
#define STREAMSDIALOG_H

#include <QDialog>
#include <QPair>
#include <QVector>

#include "stackproject.h"
#include "streamcopy.h"

class QTreeWidget;

// The sources that are on, each with the streams that its LDX file holds besides the video (EFM, AC3, RF, the 24-bit audio of a CVBS capture): the person
// ticks the ones that are copied, as they are, into the LDX file of the stack (see StreamCopy). The analogue audio is written from the fields, and is shown
// as such. What is ticked to begin with is what ld-disc-stacker takes without being told: the streams of the first source.
class StreamsDialog : public QDialog
{
    Q_OBJECT

public:
    StreamsDialog(const StackProject &project, QWidget *parent = nullptr);

    // Whether any source has a stream to choose from
    bool hasStreams() const { return !candidates.isEmpty(); }
    // The streams that are ticked: the id of the source and the track
    QVector<QPair<QString, qint32>> chosen() const;

private:
    QVector<StreamCopy::Candidate> candidates;
    QStringList ids;                // the id of each source that is on, in the order the candidates number them
    QTreeWidget *tree = nullptr;
};

#endif // STREAMSDIALOG_H
