/******************************************************************************
 * chromadecoderconfigdialog.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2019-2025 Simon Inns
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef CHROMADECODERCONFIGDIALOG_H
#define CHROMADECODERCONFIGDIALOG_H

#include <QAbstractButton>
#include <QDialog>

#include "comb.h"
#include "outputwriter.h"
#include "palcolour.h"
#include "monodecoder.h"
#include "hvddecoder.h"
#include "tbcsource.h"
#include "decodingparameters.h"

namespace Ui {
class ChromaDecoderConfigDialog;
}

class ChromaDecoderConfigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ChromaDecoderConfigDialog(QWidget *parent = nullptr);
    ~ChromaDecoderConfigDialog();

    void setConfiguration(VideoSystem system, const PalColour::Configuration &palConfiguration,
                          const Comb::Configuration &ntscConfiguration,
                          const HvdSettings &hvdSettings,
                          const MonoDecoder::MonoConfiguration &monoConfiguration,
                          const TbcSource::SourceMode &_mode,
						  const bool _isInit,
                          const OutputWriter::Configuration &outputConfiguration);
    const PalColour::Configuration &getPalConfiguration();
    // The decoder and the settings the dialog has now, as decoding parameters (the names of ld-chroma-decoder)
    DecodingParameters currentDecodingParameters() const;
    const Comb::Configuration &getNtscConfiguration();
    const HvdSettings &getHvdSettings();
    const OutputWriter::Configuration &getOutputConfiguration();

signals:
    void chromaDecoderConfigChanged();

public slots:
	void updateSourceMode(TbcSource::SourceMode mode);

private slots:
    void on_chromaGainHorizontalSlider_valueChanged(int value);
    void on_chromaPhaseHorizontalSlider_valueChanged(int value);
	void on_enableYNRCheckBox_clicked();
	void on_enableYCCombineCheckBox_clicked();

    void on_palFilterButtonGroup_buttonClicked(QAbstractButton *button);
    void on_thresholdHorizontalSlider_valueChanged(int value);
    void on_showFFTsCheckBox_clicked();
    void on_simplePALCheckBox_clicked();

    void on_decoderComboBox_activated(int index);
    void on_hvdFilterButtonGroup_buttonClicked(QAbstractButton *button);
    void on_hvdIterationsSpinBox_valueChanged(int value);
    void on_hvdStrengthHorizontalSlider_valueChanged(int value);
    void on_hvdOverlapSpinBox_valueChanged(int value);
    void on_hvdFastCheckBox_clicked();
    void on_hvdBidirectionalCheckBox_clicked();
    void on_hvdSelectiveCheckBox_clicked();
    void on_hvdPassesSpinBox_valueChanged(int value);
    void on_hvdFieldOrderComboBox_activated(int index);
    void on_hvdAccCheckBox_clicked();
    void on_hvdMonochromeCheckBox_clicked();
    void on_hvdSmoothnessSpinBox_valueChanged(double value);
    void on_hvdSymmetryCheckBox_clicked();
    void on_hvdCustomSubcarrierCheckBox_clicked();
    void on_hvdSubcarrierSpinBox_valueChanged(double value);

    void on_ntscFilterButtonGroup_buttonClicked(QAbstractButton *button);
    void on_phaseCompCheckBox_clicked();
    void on_adaptiveCheckBox_clicked();
    void on_showMapCheckBox_clicked();
    void on_adaptThresholdHorizontalSlider_valueChanged(int value);
    void on_chromaWeightHorizontalSlider_valueChanged(int value);    
    void on_cNRHorizontalSlider_valueChanged(int value);
    void on_yNRHorizontalSlider_valueChanged(int value);

private:
    Ui::ChromaDecoderConfigDialog *ui;
    VideoSystem system;
    PalColour::Configuration palConfiguration;
    Comb::Configuration ntscConfiguration;
    HvdSettings hvdSettings;
    MonoDecoder::MonoConfiguration monoConfiguration;
    OutputWriter::Configuration outputConfiguration;
	TbcSource* tbcSource = nullptr;
	TbcSource::SourceMode sourceMode;
	double ynrLevel = 0;
	bool isInit = true;
	bool combine = false;
	bool yNREnabled = true;
	bool palComb = false;    // PAL: the decoder of the list is Comb (PALcolour), not Transform

    void updateDialog();
};

#endif // CHROMADECODERCONFIGDIALOG_H
