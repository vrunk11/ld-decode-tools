/******************************************************************************
 * optionsdialogs.cpp
 * ld-stacker-gui - Disc stacking GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "optionsdialogs.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <cmath>

// ---------------------------------------------------------------------------------------------
// Stacking options

StackingOptionsDialog::StackingOptionsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Stacking options"));

    QFormLayout *form = new QFormLayout(this);

    thresholdSpin = new QSpinBox;
    thresholdSpin->setRange(0, 128);
    thresholdSpin->setToolTip(tr("For the smart modes: how far from the median, in 8-bit levels, a sample may be and still be averaged"));
    form->addRow(tr("Smart threshold:"), thresholdSpin);

    diffDodCheck = new QCheckBox(tr("Differential dropout detection"));
    diffDodCheck->setToolTip(tr("Recovers samples that every source marks as a dropout, when 3 or more sources are on"));
    form->addRow(diffDodCheck);

    passthroughCheck = new QCheckBox(tr("Pass through dropouts present on every source"));
    form->addRow(passthroughCheck);

    integrityCheck = new QCheckBox(tr("Leave out a source for a frame that has skips or dropped samples"));
    integrityCheck->setToolTip(tr("Used when the stack is processed to a file; the preview does not check it"));
    form->addRow(integrityCheck);

    reverseCheck = new QCheckBox(tr("Reverse field order (second field first)"));
    form->addRow(reverseCheck);

    const auto notify = [this]() { emit changed(); };
    connect(thresholdSpin, &QSpinBox::valueChanged, this, notify);
    for (QCheckBox *check: { diffDodCheck, passthroughCheck, integrityCheck, reverseCheck }) {
        connect(check, &QCheckBox::toggled, this, notify);
    }
}

void StackingOptionsDialog::setSettings(const StackSettings &settings)
{
    base = settings;
    const QSignalBlocker a(thresholdSpin), b(diffDodCheck), c(passthroughCheck), d(integrityCheck), e(reverseCheck);
    thresholdSpin->setValue(settings.smartThreshold);
    diffDodCheck->setChecked(settings.diffDod);
    passthroughCheck->setChecked(settings.passthrough);
    integrityCheck->setChecked(settings.integrity);
    reverseCheck->setChecked(settings.reverseFields);
}

StackSettings StackingOptionsDialog::settings() const
{
    StackSettings settings = base;
    settings.smartThreshold = thresholdSpin->value();
    settings.diffDod = diffDodCheck->isChecked();
    settings.passthrough = passthroughCheck->isChecked();
    settings.integrity = integrityCheck->isChecked();
    settings.reverseFields = reverseCheck->isChecked();
    return settings;
}

// ---------------------------------------------------------------------------------------------
// Enhance

EnhanceDialog::EnhanceDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Enhance"));
    setMinimumWidth(320);

    QVBoxLayout *layout = new QVBoxLayout(this);
    QLabel *note = new QLabel(tr("These change only how the pictures are shown, to bring out the defects: not what is stacked."));
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(note);

    QFormLayout *form = new QFormLayout;
    const auto makeSlider = [](qint32 low, qint32 high, qint32 value) {
        QSlider *slider = new QSlider(Qt::Horizontal);
        slider->setRange(low, high);
        slider->setValue(value);
        return slider;
    };
    brightnessSlider = makeSlider(-100, 100, 0);
    contrastSlider = makeSlider(25, 400, 100);
    gammaSlider = makeSlider(20, 300, 100);
    form->addRow(tr("Brightness:"), brightnessSlider);
    form->addRow(tr("Contrast:"), contrastSlider);
    form->addRow(tr("Gamma:"), gammaSlider);
    layout->addLayout(form);

    QPushButton *reset = new QPushButton(tr("Reset"));
    layout->addWidget(reset, 0, Qt::AlignRight);

    const auto notify = [this]() { emit changed(); };
    connect(brightnessSlider, &QSlider::valueChanged, this, notify);
    connect(contrastSlider, &QSlider::valueChanged, this, notify);
    connect(gammaSlider, &QSlider::valueChanged, this, notify);
    connect(reset, &QPushButton::clicked, this, [this]() {
        {
            const QSignalBlocker a(brightnessSlider), b(contrastSlider), c(gammaSlider);
            brightnessSlider->setValue(0);
            contrastSlider->setValue(100);
            gammaSlider->setValue(100);
        }
        emit changed();
    });
}

bool EnhanceDialog::isNeutral() const
{
    return brightnessSlider->value() == 0 && contrastSlider->value() == 100 && gammaSlider->value() == 100;
}

QImage EnhanceDialog::apply(const QImage &image) const
{
    if (isNeutral() || image.isNull()) return image;

    const double brightness = brightnessSlider->value() / 100.0;
    const double contrast = contrastSlider->value() / 100.0;
    const double gamma = gammaSlider->value() / 100.0;

    quint8 table[256];
    for (qint32 i = 0; i < 256; i++) {
        double value = std::pow(i / 255.0, 1.0 / gamma);
        value = (value - 0.5) * contrast + 0.5 + brightness;
        table[i] = static_cast<quint8>(qBound(0, qRound(value * 255.0), 255));
    }

    QImage result = image.convertToFormat(QImage::Format_RGB32);
    for (qint32 y = 0; y < result.height(); y++) {
        QRgb *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (qint32 x = 0; x < result.width(); x++) {
            line[x] = qRgb(table[qRed(line[x])], table[qGreen(line[x])], table[qBlue(line[x])]);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------------------------
// Chroma decoder configuration

