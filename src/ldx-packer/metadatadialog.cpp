/******************************************************************************
 * metadatadialog.cpp
 * ldx-packer - look at the metadata of a capture, change its parameters, or replace them with those of a .json or .db file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "metadatadialog.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

MetadataDialog::MetadataDialog(Document &document, int index, QWidget *parent) : QDialog(parent), m_document(document), m_index(index)
{
    setWindowTitle(tr("Metadata of %1").arg(document.at(index).name));
    resize(640, 560);
    auto *layout = new QVBoxLayout(this);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("metadataSummary"));
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    auto *hint = new QLabel(tr("The parameters of the video and of the audio, as JSON. Change them here, or replace all the metadata (the parameters and the "
                               "fields) with those of a .json or .db file of ld-decode."), this);
    hint->setWordWrap(true);
    hint->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(hint);

    m_text = new QPlainTextEdit(this);
    m_text->setObjectName(QStringLiteral("metadataText"));
    m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(m_text, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto *replace = buttons->addButton(tr("Replace from file…"), QDialogButtonBox::ActionRole);
    replace->setObjectName(QStringLiteral("metadataReplace"));
    auto *save = buttons->addButton(tr("Export all…"), QDialogButtonBox::ActionRole);
    save->setObjectName(QStringLiteral("metadataExport"));
    connect(replace, &QPushButton::clicked, this, &MetadataDialog::replaceFromFile);
    connect(save, &QPushButton::clicked, this, &MetadataDialog::exportAll);
    connect(buttons, &QDialogButtonBox::accepted, this, &MetadataDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const bool read = document.metadataOf(index, &m_root, &error);
    QApplication::restoreOverrideCursor();
    if (!read) {
        QMessageBox::warning(parent, tr("Metadata"), tr("The metadata cannot be read: %1").arg(error));
        return;
    }
    m_ok = true;
    showRoot(m_root);
}

void MetadataDialog::showRoot(const QJsonObject &root)
{
    QJsonObject parameters;
    parameters["videoParameters"] = root.value("videoParameters");
    if (root.contains("pcmAudioParameters")) parameters["pcmAudioParameters"] = root.value("pcmAudioParameters");
    m_shown = QString::fromUtf8(QJsonDocument(parameters).toJson(QJsonDocument::Indented));
    m_text->setPlainText(m_shown);

    QStringList facts;
    for (const Companion &c : m_document.at(m_index).companions) {
        if (c.kind == Ldx::Kind::Metadata && c.parameters.value("namespace").toString() != QLatin1String("ldx.rfsource") && !c.detail.isEmpty()) facts << c.detail;
    }
    const int fields = root.value("fields").toArray().size();
    QStringList parts;
    if (m_replaced) parts << tr("<b>Replaced:</b> these are the metadata of the file that was chosen.");
    parts << tr("%n field(s) described.", nullptr, fields);
    if (!facts.isEmpty() && !m_replaced) parts << tr("In the file: %1.").arg(facts.first().toHtmlEscaped());
    for (const char *name : { "ldxMap", "ldxDecoding" }) {
        if (root.contains(QLatin1String(name))) parts << tr("Has <i>%1</i> (kept as it is).").arg(QString::fromLatin1(name));
    }
    m_summary->setText(parts.join(QLatin1Char(' ')));
}

void MetadataDialog::replaceFromFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Metadata of ld-decode"), QSettings().value(QStringLiteral("lastDir")).toString(),
                                                      tr("Metadata (*.json *.db);;All files (*)"));
    if (path.isEmpty()) return;
    QJsonObject root;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool read = Document::readMetadataFile(path, &root, &error);
    QApplication::restoreOverrideCursor();
    if (!read) {
        QMessageBox::warning(this, tr("Metadata"), error);
        return;
    }
    QSettings().setValue(QStringLiteral("lastDir"), QFileInfo(path).absolutePath());
    m_root = root;
    m_replaced = true;
    showRoot(m_root);
}

void MetadataDialog::exportAll()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Write the metadata as JSON"), QSettings().value(QStringLiteral("lastDir")).toString(), tr("JSON (*.json)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(m_root).toJson(QJsonDocument::Compact)) < 0) {
        QMessageBox::warning(this, tr("Metadata"), tr("%1 cannot be written").arg(path));
    }
}

void MetadataDialog::accept()
{
    const bool edited = m_text->toPlainText() != m_shown;
    if (edited) {
        QJsonParseError parse;
        const QJsonDocument document = QJsonDocument::fromJson(m_text->toPlainText().toUtf8(), &parse);
        if (!document.isObject() || !document.object().value("videoParameters").isObject()) {
            QMessageBox::warning(this, tr("Metadata"), document.isObject() ? tr("The parameters need a \"videoParameters\" object.") : tr("This is not JSON: %1").arg(parse.errorString()));
            return;
        }
        m_root["videoParameters"] = document.object().value("videoParameters");
        if (document.object().contains("pcmAudioParameters")) m_root["pcmAudioParameters"] = document.object().value("pcmAudioParameters");
        else m_root.remove("pcmAudioParameters");
    }
    if (edited || m_replaced) m_document.setMetadata(m_index, m_root);
    QDialog::accept();
}
