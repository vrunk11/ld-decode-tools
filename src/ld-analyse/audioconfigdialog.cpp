/******************************************************************************
 * audioconfigdialog.cpp
 * ld-analyse - the window where the audio is set up: output, imported audio
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "audioconfigdialog.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

static const double ImportRate = 44100.0;

AudioConfigDialog::AudioConfigDialog(QWidget *parent) : QDialog(parent), colour(QColor(0x2a, 0x82, 0xda))
{
    setWindowTitle(tr("Audio config"));
    QVBoxLayout *top = new QVBoxLayout(this);
    QTabWidget *tabs = new QTabWidget(this);
    top->addWidget(tabs);

    // Output ------------------------------------------------------------------------------------------------------
    QWidget *output = new QWidget(this);
    QFormLayout *form = new QFormLayout(output);
    deviceCombo = new QComboBox(output);
    deviceCombo->addItem(tr("Default device"), QByteArray());
    for (const QAudioDevice &device: QMediaDevices::audioOutputs()) deviceCombo->addItem(device.description(), device.id());
    form->addRow(tr("Sound device"), deviceCombo);

    colourButton = new QPushButton(output);
    form->addRow(tr("Waveform colour"), colourButton);

    sideCheck = new QCheckBox(tr("Show the controls beside the waveform"), output);
    sideCheck->setChecked(true);
    form->addRow(QString(), sideCheck);

    QHBoxLayout *mix = new QHBoxLayout();
    leftButton = new QToolButton(output);
    leftButton->setText(tr("L"));
    leftButton->setCheckable(true);
    leftButton->setChecked(true);
    rightButton = new QToolButton(output);
    rightButton->setText(tr("R"));
    rightButton->setCheckable(true);
    rightButton->setChecked(true);
    volumeSlider = new QSlider(Qt::Horizontal, output);
    volumeSlider->setRange(0, 100);
    volumeSlider->setValue(70);
    mix->addWidget(leftButton);
    mix->addWidget(rightButton);
    mix->addWidget(volumeSlider, 1);
    form->addRow(tr("Channels and volume"), mix);
    tabs->addTab(output, tr("Output"));

    // Import ------------------------------------------------------------------------------------------------------
    QWidget *import = new QWidget(this);
    QVBoxLayout *importLayout = new QVBoxLayout(import);
    QHBoxLayout *fileRow = new QHBoxLayout();
    fileEdit = new QLineEdit(import);
    fileEdit->setPlaceholderText(tr("An audio or video file that ffmpeg reads"));
    QPushButton *browseButton = new QPushButton(tr("Browse..."), import);
    fileRow->addWidget(fileEdit, 1);
    fileRow->addWidget(browseButton);
    importLayout->addLayout(fileRow);

    QFormLayout *importForm = new QFormLayout();
    offsetSpin = new QSpinBox(import);
    offsetSpin->setRange(-60000, 60000);
    offsetSpin->setSuffix(tr(" ms"));
    offsetSpin->setToolTip(tr("The audio is this much later than the video (negative: earlier)"));
    importForm->addRow(tr("Offset"), offsetSpin);
    importLayout->addLayout(importForm);

    QHBoxLayout *buttons = new QHBoxLayout();
    useButton = new QPushButton(tr("Use"), import);
    useButton->setToolTip(tr("Read the file and play it with the video, for this session"));
    saveButton = new QPushButton(tr("Save in the file as a track"), import);
    saveButton->setToolTip(tr("Put the audio in the LDX file of the capture, as a track of its own"));
    saveButton->setEnabled(false);
    removeButton = new QPushButton(tr("Remove"), import);
    removeButton->setEnabled(false);
    buttons->addWidget(useButton);
    buttons->addWidget(saveButton);
    buttons->addWidget(removeButton);
    buttons->addStretch(1);
    importLayout->addLayout(buttons);
    status = new QLabel(import);
    status->setWordWrap(true);
    importLayout->addWidget(status);
    importLayout->addStretch(1);
    tabs->addTab(import, tr("Import"));

    QDialogButtonBox *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    top->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::close);

    connect(deviceCombo, &QComboBox::currentIndexChanged, this, [this]() { emit deviceChosen(deviceCombo->currentData().toByteArray()); });
    connect(colourButton, &QPushButton::clicked, this, &AudioConfigDialog::chooseColor);
    connect(sideCheck, &QCheckBox::toggled, this, &AudioConfigDialog::sidePanelChanged);
    const auto mixed = [this]() { emit mixChanged(leftButton->isChecked(), rightButton->isChecked(), volumeSlider->value()); };
    connect(leftButton, &QToolButton::toggled, this, mixed);
    connect(rightButton, &QToolButton::toggled, this, mixed);
    connect(volumeSlider, &QSlider::valueChanged, this, mixed);
    connect(offsetSpin, &QSpinBox::valueChanged, this, &AudioConfigDialog::offsetChanged);
    connect(browseButton, &QPushButton::clicked, this, &AudioConfigDialog::browse);
    connect(useButton, &QPushButton::clicked, this, &AudioConfigDialog::importFile);
    connect(fileEdit, &QLineEdit::returnPressed, this, &AudioConfigDialog::importFile);
    connect(saveButton, &QPushButton::clicked, this, [this]() { emit saveRequested(samples, ImportRate, name); });
    connect(removeButton, &QPushButton::clicked, this, [this]() {
        samples.clear();
        saveButton->setEnabled(false);
        removeButton->setEnabled(false);
        status->setText(tr("The imported audio is removed"));
        emit importRemoved();
    });
    setColor(colour);
}

void AudioConfigDialog::setDevice(const QByteArray &id)
{
    const QSignalBlocker blocker(deviceCombo);
    const qint32 index = deviceCombo->findData(id);
    deviceCombo->setCurrentIndex(index >= 0 ? index : 0);
}

void AudioConfigDialog::setColor(const QColor &newColour)
{
    colour = newColour;
    colourButton->setText(colour.name());
    colourButton->setStyleSheet(QString("QPushButton { background-color: %1; color: %2; }")
                                    .arg(colour.name(), colour.lightness() > 128 ? "black" : "white"));
}

void AudioConfigDialog::setSidePanel(bool shown)
{
    const QSignalBlocker blocker(sideCheck);
    sideCheck->setChecked(shown);
}

void AudioConfigDialog::setMix(bool left, bool right, qint32 volume)
{
    const QSignalBlocker a(leftButton), b(rightButton), c(volumeSlider);
    leftButton->setChecked(left);
    rightButton->setChecked(right);
    volumeSlider->setValue(volume);
}

void AudioConfigDialog::setOffsetMs(qint32 milliseconds)
{
    const QSignalBlocker blocker(offsetSpin);
    offsetSpin->setValue(milliseconds);
}

void AudioConfigDialog::setVideo(double videoSeconds, const QString &ldxFile)
{
    videoLength = videoSeconds;
    ldx = ldxFile;
    saveButton->setEnabled(!samples.isEmpty() && !ldx.isEmpty());
}

void AudioConfigDialog::setImportedName(const QString &importedName)
{
    if (!importedName.isEmpty()) status->setText(tr("In use: %1").arg(importedName));
}

void AudioConfigDialog::chooseColor()
{
    const QColor chosen = QColorDialog::getColor(colour, this, tr("Waveform colour"));
    if (!chosen.isValid()) return;
    setColor(chosen);
    emit colorChosen(chosen);
}

void AudioConfigDialog::browse()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Import audio"), QString(), tr("All files (*)"));
    if (file.isEmpty()) return;
    fileEdit->setText(file);
    importFile();
}

// ffmpeg turns the file into 16-bit stereo at 44.1 kHz, as far as the video goes
void AudioConfigDialog::importFile()
{
    const QString file = fileEdit->text().trimmed();
    if (file.isEmpty() || (ffmpeg && ffmpeg->state() != QProcess::NotRunning)) return;
    if (!QFileInfo::exists(file)) {
        status->setText(tr("There is no file %1").arg(file));
        return;
    }
    name = QFileInfo(file).fileName();
    QStringList arguments { "-v", "error", "-i", file };
    if (videoLength > 0.0) arguments << "-t" << QString::number(videoLength, 'f', 3);
    arguments << "-vn" << "-ac" << "2" << "-ar" << QString::number(static_cast<qint32>(ImportRate)) << "-f" << "s16le" << "-";

    delete ffmpeg;
    ffmpeg = new QProcess(this);
    connect(ffmpeg, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) { finishedReading(code); });
    connect(ffmpeg, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) status->setText(tr("ffmpeg could not be started: it must be installed and in the PATH"));
    });
    useButton->setEnabled(false);
    status->setText(tr("Reading %1...").arg(name));
    ffmpeg->start("ffmpeg", arguments);
}

void AudioConfigDialog::finishedReading(int exitCode)
{
    useButton->setEnabled(true);
    const QByteArray data = ffmpeg->readAllStandardOutput();
    const QString messages = QString::fromLocal8Bit(ffmpeg->readAllStandardError()).trimmed();
    if (exitCode != 0 || data.size() < 4) {
        status->setText(tr("ffmpeg could not read the audio of %1: %2").arg(name, messages.isEmpty() ? tr("nothing came out") : messages));
        return;
    }
    samples.resize(data.size() / 2);
    memcpy(samples.data(), data.constData(), static_cast<size_t>(samples.size()) * 2);
    removeButton->setEnabled(true);
    saveButton->setEnabled(!ldx.isEmpty());
    status->setText(tr("%1: %2 seconds, in use").arg(name).arg(static_cast<double>(samples.size() / 2) / ImportRate, 0, 'f', 1));
    emit imported(samples, ImportRate, name);
}
