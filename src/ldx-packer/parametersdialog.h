/******************************************************************************
 * parametersdialog.h
 * ldx-packer - the parameters of a track as JSON, to be changed
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef PARAMETERSDIALOG_H
#define PARAMETERSDIALOG_H

#include <QDialog>
#include <QJsonObject>

class QPlainTextEdit;

// What the tools know of a track (the system, the sample rate, the size of a field, the track it goes with...) is a JSON object kept with it
// (section 3.1 of the specification). It is shown whole, to be changed: a key can be given another value, added or taken out.
class ParametersDialog : public QDialog
{
    Q_OBJECT

public:
    ParametersDialog(const QString &title, const QJsonObject &parameters, QWidget *parent = nullptr);

    QJsonObject parameters() const { return m_parameters; }
    void accept() override;

private:
    QJsonObject m_parameters;
    QPlainTextEdit *m_text = nullptr;
};

#endif // PARAMETERSDIALOG_H
