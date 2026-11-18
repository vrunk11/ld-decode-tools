/******************************************************************************
 * parametersdialog.cpp
 * ldx-packer - the parameters of a track as JSON, to be changed
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "parametersdialog.h"

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>

ParametersDialog::ParametersDialog(const QString &title, const QJsonObject &parameters, QWidget *parent) : QDialog(parent), m_parameters(parameters)
{
    setWindowTitle(title);
    resize(520, 420);
    auto *layout = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("The parameters of the track, as JSON: the system, the sample rate, the size of a field, the track it goes with... "
                               "A value can be changed, a key added or taken out. \"track\" is the video track it goes with."), this);
    hint->setWordWrap(true);
    hint->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(hint);

    m_text = new QPlainTextEdit(this);
    m_text->setObjectName(QStringLiteral("parametersText"));
    m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_text->setPlainText(QString::fromUtf8(QJsonDocument(parameters).toJson(QJsonDocument::Indented)));
    layout->addWidget(m_text, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ParametersDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void ParametersDialog::accept()
{
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(m_text->toPlainText().toUtf8(), &parse);
    if (!document.isObject()) {
        QMessageBox::warning(this, windowTitle(), document.isNull() ? tr("This is not JSON: %1").arg(parse.errorString()) : tr("The parameters are an object, { \"key\": value, ... }."));
        return;
    }
    m_parameters = document.object();
    QDialog::accept();
}
