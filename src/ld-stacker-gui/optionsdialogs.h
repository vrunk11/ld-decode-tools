/******************************************************************************
 * optionsdialogs.h
 * ld-stacker-gui - Disc stacking GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef OPTIONSDIALOGS_H
#define OPTIONSDIALOGS_H

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QImage>
#include <QSlider>
#include <QSpinBox>

#include "comb.h"
#include "palcolour.h"
#include "lddecodemetadata.h"
#include "stackproject.h"

// The options of the stacking that are not the mode: the threshold of the smart modes, differential dropout
// detection, pass-through, the integrity check and the field order. Changes are applied as they are made.
class StackingOptionsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit StackingOptionsDialog(QWidget *parent = nullptr);

    void setSettings(const StackSettings &settings);
    StackSettings settings() const;

signals:
    void changed();

private:
    QSpinBox *thresholdSpin = nullptr;
    QCheckBox *diffDodCheck = nullptr;
    QCheckBox *passthroughCheck = nullptr;
    QCheckBox *integrityCheck = nullptr;
    QCheckBox *reverseCheck = nullptr;
    StackSettings base;   // what the dialog does not show (the mode)
};

// How the pictures are shown: brightness, contrast and gamma. It changes only what is on the screen, not
// what is stacked.
class EnhanceDialog : public QDialog
{
    Q_OBJECT

public:
    explicit EnhanceDialog(QWidget *parent = nullptr);

    bool isNeutral() const;
    // The picture as it is to be shown
    QImage apply(const QImage &image) const;

signals:
    void changed();

private:
    QSlider *brightnessSlider = nullptr;
    QSlider *contrastSlider = nullptr;
    QSlider *gammaSlider = nullptr;
};

// The configuration of the chroma decoders used for the pictures
class ChromaConfigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ChromaConfigDialog(QWidget *parent = nullptr);

    // The system of the sources: the settings of the other one are left unavailable
    void setSystem(VideoSystem system, bool known);
    PalColour::Configuration palConfiguration() const { return pal; }
    Comb::Configuration ntscConfiguration() const { return ntsc; }

signals:
    void changed();

private:
    void read();

    PalColour::Configuration pal;
    Comb::Configuration ntsc;

    QDoubleSpinBox *gainSpin = nullptr;
    QDoubleSpinBox *phaseSpin = nullptr;
    QDoubleSpinBox *lumaNrSpin = nullptr;

    QWidget *palBox = nullptr;
    QComboBox *palFilterCombo = nullptr;
    QDoubleSpinBox *thresholdSpin = nullptr;
    QCheckBox *simplePalCheck = nullptr;

    QWidget *ntscBox = nullptr;
    QComboBox *ntscDimensionsCombo = nullptr;
    QCheckBox *adaptiveCheck = nullptr;
    QCheckBox *phaseCompensationCheck = nullptr;
    QDoubleSpinBox *chromaNrSpin = nullptr;
    QDoubleSpinBox *adaptThresholdSpin = nullptr;
    QDoubleSpinBox *chromaWeightSpin = nullptr;
};

#endif // OPTIONSDIALOGS_H
