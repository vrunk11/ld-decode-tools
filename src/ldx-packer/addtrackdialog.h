/******************************************************************************
 * addtrackdialog.h
 * ldx-packer - add a file to a capture as a track: the file, what it is, its name, and the rate of audio
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef ADDTRACKDIALOG_H
#define ADDTRACKDIALOG_H

#include "ldx.h"

#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;

// The kind of a file is guessed from its name (.pcm and .wav audio, .efm, .ac3, .txt notes) and can be chosen; so can its name in the file and, for audio,
// the rate its samples are played at (a .wav says its own).
class AddTrackDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AddTrackDialog(const QString &capture, const QString &file = QString(), QWidget *parent = nullptr);

    QString file() const;
    Ldx::Kind kind() const;
    QString name() const;
    double sampleRate() const;
    void accept() override;

private:
    void chooseFile();
    void fileChanged();
    void kindChanged();

    QLineEdit *m_file = nullptr;
    QComboBox *m_kind = nullptr;
    QLineEdit *m_name = nullptr;
    QLabel *m_rateLabel = nullptr;
    QDoubleSpinBox *m_rate = nullptr;
    bool m_nameEdited = false;
};

#endif // ADDTRACKDIALOG_H
