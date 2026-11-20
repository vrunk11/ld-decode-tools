/******************************************************************************
 * chromadecoderconfigdialog.h
 * ld-decode-tools - the chroma decoder configuration, shared by ld-analyse and ld-stacker-gui
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
#include "lddecodemetadata.h"
#include "decodingparameters.h"

namespace Ui {
class ChromaDecoderConfigDialog;
}

class ChromaDecoderConfigDialog : public QDialog
{
    Q_OBJECT

public:
    // What the sources are: one capture, the luma of a Y/C pair, its chroma, or both
    enum SourceMode {
        ONE_SOURCE,
        LUMA_SOURCE,
        CHROMA_SOURCE,
        BOTH_SOURCES,
    };

    explicit ChromaDecoderConfigDialog(QWidget *parent = nullptr);
    ~ChromaDecoderConfigDialog();

    // SECAM and NTSC-VHD, which have the lines of PAL and of NTSC (a VideoDisc can only be decoded by HVD, with its own subcarrier); before setConfiguration
    void setSystemVariant(const QString &variant) { systemVariant = variant; }
    // The sources are stacked (ld-stacker-gui): the decoder is chosen as for a stack, which is 2D for tapes and 3D for LaserDiscs, and not for one capture
    void setStack(bool stacked) { isStack = stacked; }
    // The source that is decoded: its video parameters, and how its metadata keep that it is to be decoded (null: they do not); before setConfiguration
    void setSource(const LdDecodeMetaData::VideoParameters &parameters, const DecodingParameters *stored)
    {
        sourceParameters = parameters;
        hasStored = stored != nullptr;
        storedParameters = stored ? *stored : DecodingParameters();
    }
    void setConfiguration(VideoSystem system, const PalColour::Configuration &palConfiguration,
                          const Comb::Configuration &ntscConfiguration,
                          const HvdSettings &hvdSettings,
                          const MonoDecoder::MonoConfiguration &monoConfiguration,
                          const SourceMode &_mode,
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
    // The box that adds the chroma to the luma of a Y/C pair was clicked
    void combineChanged(bool combine);

public slots:
	void updateSourceMode(SourceMode mode);

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
    void on_hvdGpuCheckBox_clicked();
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
    QString systemVariant;      // "secam" or "ntsc-vhd" (the lines are those of system)
    PalColour::Configuration palConfiguration;
    Comb::Configuration ntscConfiguration;
    HvdSettings hvdSettings;
    MonoDecoder::MonoConfiguration monoConfiguration;
    OutputWriter::Configuration outputConfiguration;

	SourceMode sourceMode = ONE_SOURCE;
    LdDecodeMetaData::VideoParameters sourceParameters;      // of the source, for the decoder it is decoded with by default
    bool hasStored = false;
    DecodingParameters storedParameters;                      // how the capture was chosen to be decoded, when its metadata keep it
	double ynrLevel = 0;
	bool isInit = true;
    bool isStack = false;
	bool combine = false;
	bool yNREnabled = true;
	bool palComb = false;    // PAL: the decoder of the list is Comb (PALcolour), not Transform

    void updateDialog();
};

#endif // CHROMADECODERCONFIGDIALOG_H
