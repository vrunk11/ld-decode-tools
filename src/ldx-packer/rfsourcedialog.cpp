/******************************************************************************
 * rfsourcedialog.cpp
 * ldx-packer - where a capture comes from: what is known of the RF file it was decoded from
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "rfsourcedialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

RfSourceDialog::RfSourceDialog(const QByteArray &json, const QString &captureName, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Where %1 comes from").arg(captureName));
    setObjectName(QStringLiteral("rfSourceDialog"));
    auto *layout = new QVBoxLayout(this);
    auto *about = new QLabel(tr("What is known of the RF file the capture was decoded from (the track ldx.rfsource). It is information to trace the capture, and has no other use. "
                                "The names are those of an .rfx archive of RF-tools: what an archive says is copied as it is."), this);
    about->setWordWrap(true);
    about->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(about);

    auto *form = new QFormLayout;
    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(tr("the name of the file (no directory)"));
    form->addRow(tr("File"), m_name);
    m_size = new QLineEdit(this);
    m_size->setPlaceholderText(tr("bytes"));
    form->addRow(tr("Size"), m_size);
    m_sha256 = new QLineEdit(this);
    m_sha256->setPlaceholderText(tr("SHA-256 of its bytes"));
    form->addRow(tr("SHA-256"), m_sha256);
    m_format = new QComboBox(this);
    m_format->addItem(tr("Not said"), QString());
    for (const QString &format : { QStringLiteral("s16"), QStringLiteral("r16"), QStringLiteral("s8"), QStringLiteral("r8"), QStringLiteral("lds") }) m_format->addItem(format, format);
    form->addRow(tr("Sample format"), m_format);
    m_bits = new QSpinBox(this);
    m_bits->setRange(0, 32);
    m_bits->setSpecialValueText(tr("Not said"));
    m_bits->setToolTip(tr("The bits the samples carry (the rest of each 16-bit sample is zero)"));
    form->addRow(tr("Bit depth"), m_bits);
    m_rate = new QDoubleSpinBox(this);
    m_rate->setDecimals(6);
    m_rate->setRange(0.0, 10000.0);
    m_rate->setSuffix(tr(" MHz"));
    m_rate->setSpecialValueText(tr("Not said"));
    m_rate->setToolTip(tr("The sample rate of the RF capture (not the one of the video track, which is that of the TBC)"));
    form->addRow(tr("RF sample rate"), m_rate);
    m_fileTime = new QLineEdit(this);
    m_fileTime->setPlaceholderText(QStringLiteral("2023-05-16T18:03:54Z"));
    m_fileTime->setToolTip(tr("When the file was last changed, UTC, ISO 8601"));
    form->addRow(tr("File date"), m_fileTime);
    m_origin = new QLineEdit(this);
    m_origin->setPlaceholderText(tr("the compressed file it was itself made from (.flac, .ldf)"));
    form->addRow(tr("Made from"), m_origin);
    m_archive = new QLineEdit(this);
    m_archive->setPlaceholderText(tr("the .rfx archive it came from"));
    form->addRow(tr("Archive"), m_archive);
    m_signal = new QComboBox(this);
    m_signal->addItem(tr("Not said"), QString());
    m_signal->addItem(tr("Composite (CVBS)"), QStringLiteral("cvbs"));
    m_signal->addItem(tr("Separate luma and chroma (Y/C)"), QStringLiteral("yc"));
    form->addRow(tr("Signal"), m_signal);
    layout->addLayout(form);

    layout->addWidget(new QLabel(tr("Files that were beside the RF file"), this));
    m_sidecars = new QListWidget(this);
    m_sidecars->setMaximumHeight(80);
    layout->addWidget(m_sidecars);
    auto *sidecarButtons = new QHBoxLayout;
    auto *removeSidecar = new QPushButton(tr("Take the file out"), this);
    removeSidecar->setEnabled(false);
    sidecarButtons->addWidget(removeSidecar);
    sidecarButtons->addStretch(1);
    layout->addLayout(sidecarButtons);
    connect(m_sidecars, &QListWidget::currentRowChanged, this, [removeSidecar](int row) { removeSidecar->setEnabled(row >= 0); });
    connect(removeSidecar, &QPushButton::clicked, this, [this]() {
        const int row = m_sidecars->currentRow();
        if (row < 0 || row >= m_sidecarArray.size()) return;
        m_sidecarArray.removeAt(row);
        delete m_sidecars->takeItem(row);
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto *replace = buttons->addButton(tr("Replace with a .json file…"), QDialogButtonBox::ActionRole);
    auto *clear = buttons->addButton(tr("Clear"), QDialogButtonBox::ActionRole);
    connect(replace, &QPushButton::clicked, this, &RfSourceDialog::replaceFromFile);
    connect(clear, &QPushButton::clicked, this, [this]() { display(QJsonObject()); });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    display(QJsonDocument::fromJson(json).object());
}

// The window shows an object (what it does not show is kept to be given back)
void RfSourceDialog::display(const QJsonObject &object)
{
    m_object = object;
    m_name->setText(object.value("name").toString());
    m_size->setText(object.contains("size") ? QString::number(static_cast<qint64>(object.value("size").toDouble())) : QString());
    m_sha256->setText(object.value("sha256").toString());
    m_format->setCurrentIndex(std::max(0, m_format->findData(object.value("fmt").toString())));
    m_bits->setValue(object.value("bits").toInt(0));
    m_rate->setValue(object.value("rate_hz").toDouble(0.0) / 1.0e6);
    m_fileTime->setText(object.value("file_time").toString());
    m_origin->setText(object.value("origin").toString());
    m_archive->setText(object.value("archive").toString());
    m_signal->setCurrentIndex(std::max(0, m_signal->findData(object.value("signal").toString())));
    m_sidecarArray = object.value("sidecars").toArray();
    m_sidecars->clear();
    for (const QJsonValue &sidecar : m_sidecarArray) {
        m_sidecars->addItem(tr("%1 (%2 bytes)").arg(sidecar.toObject().value("name").toString()).arg(sidecar.toObject().value("data").toArray().size()));
    }
}

void RfSourceDialog::replaceFromFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Where the capture comes from"), QString(), tr("JSON (*.json);;All files (*)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Where the capture comes from"), tr("%1 cannot be opened.").arg(path));
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject() || document.object().isEmpty()) {
        QMessageBox::warning(this, tr("Where the capture comes from"), tr("%1 is not a JSON object.").arg(path));
        return;
    }
    display(document.object());
}

// What the window says now, on top of what it does not show
QJsonObject RfSourceDialog::collect() const
{
    QJsonObject object = m_object;
    const auto putText = [&object](const char *key, const QString &text) {
        if (text.trimmed().isEmpty()) object.remove(QLatin1String(key));
        else object[QLatin1String(key)] = text.trimmed();
    };
    putText("name", m_name->text());
    putText("sha256", m_sha256->text());
    putText("file_time", m_fileTime->text());
    putText("origin", m_origin->text());
    putText("archive", m_archive->text());
    putText("fmt", m_format->currentData().toString());
    putText("signal", m_signal->currentData().toString());
    bool ok = false;
    const qint64 size = m_size->text().trimmed().toLongLong(&ok);
    if (ok && size >= 0) object["size"] = static_cast<double>(size);
    else object.remove("size");
    if (m_bits->value() > 0) object["bits"] = m_bits->value();
    else object.remove("bits");
    if (m_rate->value() > 0.0) object["rate_hz"] = m_rate->value() * 1.0e6;
    else object.remove("rate_hz");
    if (m_sidecarArray.isEmpty()) object.remove("sidecars");
    else object["sidecars"] = m_sidecarArray;
    return object;
}

QByteArray RfSourceDialog::json() const
{
    const QJsonObject object = collect();
    return object.isEmpty() ? QByteArray() : QJsonDocument(object).toJson(QJsonDocument::Compact);
}
