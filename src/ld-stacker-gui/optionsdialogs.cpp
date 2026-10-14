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

ChromaConfigDialog::ChromaConfigDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Chroma decoder configuration"));
    setMinimumWidth(380);

    // As ld-analyse starts them
    pal.chromaFilter = PalColour::transform2DFilter;
    ntsc.dimensions = 2;

    QVBoxLayout *layout = new QVBoxLayout(this);

    QFormLayout *common = new QFormLayout;
    const auto makeSpin = [](double low, double high, double step, qint32 decimals, double value, const QString &suffix = QString()) {
        QDoubleSpinBox *spin = new QDoubleSpinBox;
        spin->setRange(low, high);
        spin->setSingleStep(step);
        spin->setDecimals(decimals);
        spin->setValue(value);
        spin->setSuffix(suffix);
        return spin;
    };
    gainSpin = makeSpin(0, 2, 0.05, 2, 1.0);
    phaseSpin = makeSpin(-180, 180, 1, 1, 0.0, QString::fromUtf8("\xC2\xB0"));
    lumaNrSpin = makeSpin(0, 10, 0.1, 1, 0.0, tr(" IRE"));
    common->addRow(tr("Chroma gain:"), gainSpin);
    common->addRow(tr("Chroma phase:"), phaseSpin);
    common->addRow(tr("Luma noise reduction:"), lumaNrSpin);
    layout->addLayout(common);

    palBox = new QWidget;
    QFormLayout *palForm = new QFormLayout(palBox);
    palForm->setContentsMargins(0, 8, 0, 0);
    palFilterCombo = new QComboBox;
    palFilterCombo->addItem(tr("PALcolour (2D)"), static_cast<qint32>(PalColour::palColourFilter));
    palFilterCombo->addItem(tr("Transform PAL 2D"), static_cast<qint32>(PalColour::transform2DFilter));
    palFilterCombo->addItem(tr("Transform PAL 3D"), static_cast<qint32>(PalColour::transform3DFilter));
    palFilterCombo->setCurrentIndex(1);
    thresholdSpin = makeSpin(0, 1, 0.05, 2, 0.4);
    simplePalCheck = new QCheckBox(tr("Simple PAL (no chroma delay line)"));
    palForm->addRow(tr("PAL filter:"), palFilterCombo);
    palForm->addRow(tr("Transform threshold:"), thresholdSpin);
    palForm->addRow(simplePalCheck);
    layout->addWidget(palBox);

    ntscBox = new QWidget;
    QFormLayout *ntscForm = new QFormLayout(ntscBox);
    ntscForm->setContentsMargins(0, 8, 0, 0);
    ntscDimensionsCombo = new QComboBox;
    ntscDimensionsCombo->addItem(tr("1D"), 1);
    ntscDimensionsCombo->addItem(tr("2D"), 2);
    ntscDimensionsCombo->addItem(tr("3D"), 3);
    ntscDimensionsCombo->setCurrentIndex(1);
    adaptiveCheck = new QCheckBox(tr("Adaptive filter"));
    adaptiveCheck->setChecked(true);
    phaseCompensationCheck = new QCheckBox(tr("Phase compensation (tape)"));
    chromaNrSpin = makeSpin(0, 10, 0.1, 1, 0.0, tr(" IRE"));
    adaptThresholdSpin = makeSpin(0.1, 3, 0.1, 2, 1.0);
    chromaWeightSpin = makeSpin(0, 2, 0.1, 2, 1.0);
    ntscForm->addRow(tr("NTSC filter:"), ntscDimensionsCombo);
    ntscForm->addRow(adaptiveCheck);
    ntscForm->addRow(phaseCompensationCheck);
    ntscForm->addRow(tr("Adaptive threshold:"), adaptThresholdSpin);
    ntscForm->addRow(tr("Chroma weight:"), chromaWeightSpin);
    ntscForm->addRow(tr("Chroma noise reduction:"), chromaNrSpin);
    layout->addWidget(ntscBox);

    QLabel *note = new QLabel(tr("The 3D filters look at the frames either side too, so each of them is stacked as well, which is slower."));
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(note);

    for (QDoubleSpinBox *spin: { gainSpin, phaseSpin, lumaNrSpin, thresholdSpin, chromaNrSpin, adaptThresholdSpin, chromaWeightSpin }) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this]() { read(); });
    }
    for (QCheckBox *check: { simplePalCheck, adaptiveCheck, phaseCompensationCheck }) {
        connect(check, &QCheckBox::toggled, this, [this]() { read(); });
    }
    connect(palFilterCombo, &QComboBox::currentIndexChanged, this, [this]() { read(); });
    connect(ntscDimensionsCombo, &QComboBox::currentIndexChanged, this, [this]() { read(); });
}

void ChromaConfigDialog::setSystem(VideoSystem system, bool known)
{
    palBox->setEnabled(!known || system == PAL || system == PAL_M);
    ntscBox->setEnabled(!known || system == NTSC);
}

// What the controls say is the configuration
void ChromaConfigDialog::read()
{
    pal.chromaGain = ntsc.chromaGain = gainSpin->value();
    pal.chromaPhase = ntsc.chromaPhase = phaseSpin->value();
    pal.yNRLevel = ntsc.yNRLevel = lumaNrSpin->value();

    pal.chromaFilter = static_cast<PalColour::ChromaFilterMode>(palFilterCombo->currentData().toInt());
    pal.transformThreshold = thresholdSpin->value();
    pal.simplePAL = simplePalCheck->isChecked();

    ntsc.dimensions = ntscDimensionsCombo->currentData().toInt();
    ntsc.adaptive = adaptiveCheck->isChecked();
    ntsc.phaseCompensation = phaseCompensationCheck->isChecked();
    ntsc.cNRLevel = chromaNrSpin->value();
    ntsc.adaptThreshold = adaptThresholdSpin->value();
    ntsc.chromaWeight = chromaWeightSpin->value();

    emit changed();
}
