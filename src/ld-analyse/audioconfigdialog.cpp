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
#include <QPlainTextEdit>
#include <QFontDatabase>
#include <QPushButton>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

static const double ImportRate = 44100.0;

AudioConfigDialog::AudioConfigDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Audio panel"));
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

    waveformCheck = new QCheckBox(tr("Show the waveform under the video"), output);
    waveformCheck->setChecked(true);
    form->addRow(QString(), waveformCheck);

    sideCheck = new QCheckBox(tr("Show the controls beside the waveform"), output);
    sideCheck->setChecked(true);
    form->addRow(QString(), sideCheck);

    QHBoxLayout *mix = new QHBoxLayout();
    leftButton = new QToolButton(output);
    leftButton->setText(tr("L"));
    leftButton->setCheckable(true);
    leftButton->setChecked(false);
    rightButton = new QToolButton(output);
    rightButton->setText(tr("R"));
    rightButton->setCheckable(true);
    rightButton->setChecked(false);
    muteButton = new QToolButton(output);
    muteButton->setText(tr("M"));
    muteButton->setCheckable(true);
    muteButton->setToolTip(tr("Mute"));
    volumeSlider = new QSlider(Qt::Horizontal, output);
    volumeSlider->setRange(0, 100);
    volumeSlider->setValue(70);
    leftButton->setToolTip(tr("Mute the left channel"));
    rightButton->setToolTip(tr("Mute the right channel"));
    mix->addWidget(leftButton);
    mix->addWidget(rightButton);
    mix->addWidget(muteButton);
    mix->addWidget(volumeSlider, 1);
    form->addRow(tr("Channels and volume"), mix);
    tabs->addTab(output, tr("Output"));

    // Style -------------------------------------------------------------------------------------------------------
    QWidget *style = new QWidget(this);
    QFormLayout *styleForm = new QFormLayout(style);
    styleCombo = new QComboBox(style);
    styleCombo->addItem(tr("Filled"), 0);
    styleCombo->addItem(tr("Outline"), 1);
    styleCombo->addItem(tr("Bars"), 2);
    densityCombo = new QComboBox(style);
    densityCombo->addItem(tr("Compact"), 1);
    densityCombo->addItem(tr("Large"), 0);
    densityCombo->addItem(tr("Ultra compact (one line of buttons)"), 2);
    styleForm->addRow(tr("Height"), densityCombo);
    styleForm->addRow(tr("Waveform design"), styleCombo);
    wellCheck = new QCheckBox(tr("Waveform in a box, set into the panel"), style);
    wellCheck->setChecked(true);
    styleForm->addRow(QString(), wellCheck);
    QLabel *colourHelp = new QLabel(tr("The colours of the waveform (analogue audio, audio from EFM) are in the theme: View > Theme > Customize."), style);
    colourHelp->setWordWrap(true);
    styleForm->addRow(colourHelp);
    tabs->addTab(style, tr("Style"));

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

    // EFM -------------------------------------------------------------------------------------------------------
    QWidget *efm = new QWidget(this);
    QVBoxLayout *efmLayout = new QVBoxLayout(efm);
    efmSource = new QLabel(efm);
    efmSource->setWordWrap(true);
    efmLayout->addWidget(efmSource);
    noTimecodesCheck = new QCheckBox(tr("EFM without timecodes (the usual one of a LaserDisc)"), efm);
    noTimecodesCheck->setChecked(true);
    concealCheck = new QCheckBox(tr("Conceal the errors of the audio"), efm);
    concealCheck->setChecked(true);
    padCheck = new QCheckBox(tr("Begin the audio at disc time 00:00"), efm);
    efmLayout->addWidget(noTimecodesCheck);
    efmLayout->addWidget(concealCheck);
    efmLayout->addWidget(padCheck);
    // One decoder at a time: the choice of the decoder, and its button
    decoderCombo = new QComboBox(efm);
    decoderCombo->addItem(tr("ld-process-efm"), 1);
    decoderCombo->addItem(tr("efm-decoder (f2, d24, audio)"), 0);
    QHBoxLayout *decoderRow = new QHBoxLayout();
    decoderRow->addWidget(new QLabel(tr("Decoder"), efm));
    decoderRow->addWidget(decoderCombo, 1);
    decodeButton = new QPushButton(tr("Decode"), efm);
    decodeButton->setEnabled(false);
    stopButton = new QPushButton(tr("Stop"), efm);
    stopButton->setEnabled(false);
    decoderRow->addWidget(decodeButton);
    decoderRow->addWidget(stopButton);
    efmLayout->addLayout(decoderRow);
    decodeStatusLabel = new QLabel(efm);
    decodeStatusLabel->setWordWrap(true);
    efmLayout->addWidget(decodeStatusLabel);
    connect(decodeButton, &QPushButton::clicked, this, [this]() {
        emit decodeRequested(decoderCombo->currentData().toInt(), noTimecodesCheck->isChecked(), concealCheck->isChecked(), padCheck->isChecked());
    });
    connect(stopButton, &QPushButton::clicked, this, [this]() { emit decodeStopRequested(decoderCombo->currentData().toInt()); });
    connect(decoderCombo, &QComboBox::currentIndexChanged, this, [this]() { showDecoderState(); emit decoderChosen(decoderCombo->currentData().toInt()); });
    QLabel *efmHelp = new QLabel(tr("One decoder runs at a time: starting one stops the other. The same choice is in the source menu of the waveform."), efm);
    efmHelp->setWordWrap(true);
    efmLayout->addWidget(efmHelp);
    statistics = new QPlainTextEdit(efm);
    statistics->setReadOnly(true);
    statistics->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    statistics->setFont(mono);
    statistics->setMinimumHeight(200);
    statistics->setPlaceholderText(tr("Statistics of the sound and of the decoders"));
    efmLayout->addWidget(statistics, 1);
    tabs->addTab(efm, tr("EFM"));

    QDialogButtonBox *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    top->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::close);

    connect(deviceCombo, &QComboBox::currentIndexChanged, this, [this]() { emit deviceChosen(deviceCombo->currentData().toByteArray()); });
    connect(waveformCheck, &QCheckBox::toggled, this, &AudioConfigDialog::waveformShownChanged);
    connect(muteButton, &QToolButton::toggled, this, &AudioConfigDialog::mutedChanged);
    connect(densityCombo, &QComboBox::currentIndexChanged, this, [this]() { emit densityChosen(densityCombo->currentData().toInt()); });
    connect(wellCheck, &QCheckBox::toggled, this, &AudioConfigDialog::wellChanged);
    connect(styleCombo, &QComboBox::currentIndexChanged, this, [this]() { emit styleChosen(styleCombo->currentData().toInt()); });
    connect(sideCheck, &QCheckBox::toggled, this, &AudioConfigDialog::sidePanelChanged);
    const auto mixed = [this]() { emit mixChanged(!leftButton->isChecked(), !rightButton->isChecked(), volumeSlider->value()); };
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
}

void AudioConfigDialog::setDevice(const QByteArray &id)
{
    const QSignalBlocker blocker(deviceCombo);
    const qint32 index = deviceCombo->findData(id);
    deviceCombo->setCurrentIndex(index >= 0 ? index : 0);
}

void AudioConfigDialog::setMuted(bool muted)
{
    const QSignalBlocker blocker(muteButton);
    muteButton->setChecked(muted);
}

void AudioConfigDialog::setWaveformShown(bool shown)
{
    const QSignalBlocker blocker(waveformCheck);
    waveformCheck->setChecked(shown);
}

void AudioConfigDialog::setStyle(qint32 style)
{
    const QSignalBlocker blocker(styleCombo);
    styleCombo->setCurrentIndex(qMax(0, styleCombo->findData(style)));
}

void AudioConfigDialog::setSidePanel(bool shown)
{
    const QSignalBlocker blocker(sideCheck);
    sideCheck->setChecked(shown);
}

void AudioConfigDialog::setMix(bool left, bool right, qint32 volume)
{
    const QSignalBlocker a(leftButton), b(rightButton), c(volumeSlider);
    leftButton->setChecked(!left);
    rightButton->setChecked(!right);
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

void AudioConfigDialog::setEfmSource(const QString &description)
{
    hasEfm = !description.isEmpty();
    efmSource->setText(description.isEmpty() ? tr("The capture has no EFM to decode") : tr("EFM of %1").arg(description));
    showDecoderState();
}

void AudioConfigDialog::setDecodeStatus(qint32 decoder, const QString &text, bool isRunning)
{
    if (decoder < 0 || decoder > 1) return;
    statusText[decoder] = text;
    running[decoder] = isRunning;
    showDecoderState();
}

void AudioConfigDialog::setActiveDecoder(qint32 decoder)
{
    const QSignalBlocker blocker(decoderCombo);
    const qint32 index = decoderCombo->findData(decoder);
    if (index >= 0) decoderCombo->setCurrentIndex(index);
    showDecoderState();
}

// The buttons and the line of the decoder that is chosen
void AudioConfigDialog::showDecoderState()
{
    const qint32 decoder = decoderCombo->currentData().toInt();
    decodeStatusLabel->setText(statusText[decoder]);
    stopButton->setEnabled(running[decoder]);
    decodeButton->setEnabled(!running[decoder] && hasEfm);
}

bool AudioConfigDialog::efmWithoutTimecodes() const { return noTimecodesCheck->isChecked(); }
bool AudioConfigDialog::efmConceal() const { return concealCheck->isChecked(); }
bool AudioConfigDialog::efmPad() const { return padCheck->isChecked(); }

void AudioConfigDialog::setDensity(qint32 density)
{
    const QSignalBlocker blocker(densityCombo);
    densityCombo->setCurrentIndex(qMax(0, densityCombo->findData(density)));
}

void AudioConfigDialog::setStatistics(const QString &text)
{
    if (statistics->toPlainText() == text) return;
    // (the view stays where it is: the last lines are followed only if it was at the end)
    QScrollBar *bar = statistics->verticalScrollBar();
    const bool atEnd = bar->value() >= bar->maximum() - 2;
    const qint32 value = bar->value();
    statistics->setPlainText(text);
    bar->setValue(atEnd ? bar->maximum() : value);
}

qint32 AudioConfigDialog::currentDecoder() const { return decoderCombo->currentData().toInt(); }

void AudioConfigDialog::setWell(bool on)
{
    const QSignalBlocker blocker(wellCheck);
    wellCheck->setChecked(on);
}
