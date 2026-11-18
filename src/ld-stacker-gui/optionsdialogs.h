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


#endif // OPTIONSDIALOGS_H
