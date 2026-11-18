/******************************************************************************
 * rfsourcedialog.h
 * ldx-packer - where a capture comes from: what is known of the RF file it was decoded from
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef RFSOURCEDIALOG_H
#define RFSOURCEDIALOG_H

#include <QByteArray>
#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>

class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QListWidget;
class QSpinBox;

// The track ldx.rfsource of a capture (section 8.4 of the specification): the entry of the capture in an .rfx archive of RF-tools, as that spells it. It is information to trace
// the capture and has no other use. The members the window does not know are kept as they are.
class RfSourceDialog : public QDialog
{
    Q_OBJECT
public:
    // json: what the capture has now (compact JSON, empty when it has nothing)
    RfSourceDialog(const QByteArray &json, const QString &captureName, QWidget *parent = nullptr);

    // What the capture has when the window is accepted: compact JSON, empty for nothing (every field empty)
    QByteArray json() const;

private:
    void display(const QJsonObject &object);
    void replaceFromFile();
    QJsonObject collect() const;

    QJsonObject m_object;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_size = nullptr;
    QLineEdit *m_sha256 = nullptr;
    QComboBox *m_format = nullptr;
    QSpinBox *m_bits = nullptr;
    QDoubleSpinBox *m_rate = nullptr;
    QLineEdit *m_fileTime = nullptr;
    QLineEdit *m_origin = nullptr;
    QLineEdit *m_archive = nullptr;
    QComboBox *m_signal = nullptr;
    QListWidget *m_sidecars = nullptr;
    QJsonArray m_sidecarArray;      // the files that were beside the RF file (they can be taken out, not changed)
};

#endif // RFSOURCEDIALOG_H
