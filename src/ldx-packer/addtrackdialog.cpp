/******************************************************************************
 * addtrackdialog.cpp
 * ldx-packer - add a file to a capture as a track: the file, what it is, its name, and the rate of audio
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "addtrackdialog.h"

#include "document.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

AddTrackDialog::AddTrackDialog(const QString &capture, const QString &file, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Add a track to %1").arg(capture));
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    auto *fileRow = new QHBoxLayout;
    m_file = new QLineEdit(file, this);
    m_file->setObjectName(QStringLiteral("trackFile"));
    auto *browse = new QPushButton(tr("Browse…"), this);
    fileRow->addWidget(m_file, 1);
    fileRow->addWidget(browse);
    form->addRow(tr("File"), fileRow);

    m_kind = new QComboBox(this);
    m_kind->setObjectName(QStringLiteral("trackKind"));
    m_kind->addItem(tr("Audio (16-bit stereo PCM, or a .wav)"), static_cast<int>(Ldx::Kind::Pcm));
    m_kind->addItem(tr("EFM t-values"), static_cast<int>(Ldx::Kind::Efm));
    m_kind->addItem(tr("AC3 data"), static_cast<int>(Ldx::Kind::Ac3));
    m_kind->addItem(tr("Notes (a text)"), static_cast<int>(Ldx::Kind::Annotations));
    // (the chroma of a Y/C capture is a track of the kind of the capture: Tbc stands for it here)
    m_kind->addItem(tr("Chroma of a Y/C capture (_chroma.tbc, .tbcc, .cvbsc)"), static_cast<int>(Ldx::Kind::Tbc));
    form->addRow(tr("What it is"), m_kind);

    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("trackNewName"));
    form->addRow(tr("Name"), m_name);

    m_rate = new QDoubleSpinBox(this);
    m_rate->setObjectName(QStringLiteral("trackNewRate"));
    m_rate->setDecimals(0);
    m_rate->setRange(8000.0, 192000.0);
    m_rate->setValue(44100.0);
    m_rate->setSuffix(tr(" Hz"));
    m_rate->setToolTip(tr("The rate the samples are played at: 44100 Hz for the audio of a capture. A .wav says its own."));
    m_rateLabel = new QLabel(tr("Sample rate"), this);
    form->addRow(m_rateLabel, m_rate);
    layout->addLayout(form);

    auto *note = new QLabel(tr("The file is written again with the track in it: audio, EFM and AC3 have data all along the capture, so they go in with the rest."), this);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(note);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(browse, &QPushButton::clicked, this, &AddTrackDialog::chooseFile);
    connect(m_file, &QLineEdit::editingFinished, this, &AddTrackDialog::fileChanged);
    connect(m_kind, &QComboBox::currentIndexChanged, this, [this](int) { kindChanged(); });
    connect(m_name, &QLineEdit::textEdited, this, [this] { m_nameEdited = true; });
    connect(buttons, &QDialogButtonBox::accepted, this, &AddTrackDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    fileChanged();
}

QString AddTrackDialog::file() const { return m_file->text().trimmed(); }
Ldx::Kind AddTrackDialog::kind() const { return static_cast<Ldx::Kind>(m_kind->currentData().toInt()); }
QString AddTrackDialog::name() const { return m_name->text().trimmed(); }
double AddTrackDialog::sampleRate() const { return m_rate->value(); }

void AddTrackDialog::chooseFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("File to add as a track"), QSettings().value(QStringLiteral("lastDir")).toString(),
                                                      tr("Audio, EFM, AC3, notes and chroma (*.pcm *.wav *.efm *.ac3 *.txt *.tbc *.tbcc *.cvbsc);;All files (*)"));
    if (path.isEmpty()) return;
    QSettings().setValue(QStringLiteral("lastDir"), QFileInfo(path).absolutePath());
    m_file->setText(path);
    fileChanged();
}

// What the file is, from its name; the name of the track is the name of the file until it is changed
void AddTrackDialog::fileChanged()
{
    const QString path = file();
    const QString suffix = QFileInfo(path).suffix().toLower();
    int index = -1;
    if (suffix == QLatin1String("pcm") || suffix == QLatin1String("wav")) index = m_kind->findData(static_cast<int>(Ldx::Kind::Pcm));
    else if (suffix == QLatin1String("efm")) index = m_kind->findData(static_cast<int>(Ldx::Kind::Efm));
    else if (suffix == QLatin1String("ac3")) index = m_kind->findData(static_cast<int>(Ldx::Kind::Ac3));
    else if (suffix == QLatin1String("txt") || suffix == QLatin1String("md")) index = m_kind->findData(static_cast<int>(Ldx::Kind::Annotations));
    else if (suffix == QLatin1String("tbc") || suffix == QLatin1String("tbcc") || suffix == QLatin1String("cvbsc")) index = m_kind->findData(static_cast<int>(Ldx::Kind::Tbc));
    if (index >= 0) m_kind->setCurrentIndex(index);
    if (!m_nameEdited) m_name->setText(QFileInfo(path).fileName());
    if (suffix == QLatin1String("wav")) {
        double rate = 0.0;
        if (Document::wavRate(path, &rate, nullptr)) m_rate->setValue(rate);
    }
    kindChanged();
}

void AddTrackDialog::kindChanged()
{
    const bool audio = kind() == Ldx::Kind::Pcm;
    m_rate->setVisible(audio);
    m_rateLabel->setVisible(audio);
}

void AddTrackDialog::accept()
{
    if (!QFileInfo(file()).isFile()) {
        QMessageBox::warning(this, windowTitle(), tr("Choose the file to add."));
        return;
    }
    QDialog::accept();
}
