/******************************************************************************
 * chromadecoderconfigdialog.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2019-2025 Simon Inns
 * SPDX-FileCopyrightText: 2020-2022 Adam Sampson
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "chromadecoderconfigdialog.h"
#include "ui_chromadecoderconfigdialog.h"
#include "mainwindow.h"

#include <cmath>

/*
 * These two functions provide a non-linear mapping for sliders that control
 * phase adjustments in degrees. The maximum range is from -180 to +180
 * degrees, but phase errors are usually < 10 degrees so we need more precise
 * adjustment in the middle.
 */

static constexpr double DEGREE_SLIDER_POWER = 3.0;
static constexpr qint32 DEGREE_SLIDER_SCALE = 1000;

static double degreesToSliderPos(double degrees) {
    double sliderPos = pow(abs(degrees) / 180, 1 / DEGREE_SLIDER_POWER) * DEGREE_SLIDER_SCALE;
    if (degrees < 0) {
        return -sliderPos;
    } else {
        return sliderPos;
    }
}

static double sliderPosToDegrees(double sliderPos) {
    double degrees = pow(abs(sliderPos) / DEGREE_SLIDER_SCALE, DEGREE_SLIDER_POWER) * 180;
    if (sliderPos < 0) {
        return -degrees;
    } else {
        return degrees;
    }
}

ChromaDecoderConfigDialog::ChromaDecoderConfigDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::ChromaDecoderConfigDialog)
{
    ui->setupUi(this);
    setWindowFlags(Qt::Window);

    ui->chromaGainHorizontalSlider->setMinimum(0);
    ui->chromaGainHorizontalSlider->setMaximum(200);

    ui->chromaPhaseHorizontalSlider->setMinimum(-DEGREE_SLIDER_SCALE);
    ui->chromaPhaseHorizontalSlider->setMaximum(DEGREE_SLIDER_SCALE);

    ui->thresholdHorizontalSlider->setMinimum(0);
    ui->thresholdHorizontalSlider->setMaximum(100);

    ui->cNRHorizontalSlider->setMinimum(0);
    ui->cNRHorizontalSlider->setMaximum(100);

    ui->adaptThresholdHorizontalSlider->setMinimum(10);
    ui->adaptThresholdHorizontalSlider->setMaximum(200);

    ui->chromaWeightHorizontalSlider->setMinimum(0);
    ui->chromaWeightHorizontalSlider->setMaximum(200);

    ui->yNRHorizontalSlider->setMinimum(0);
    ui->yNRHorizontalSlider->setMaximum(100);
    
	//get tbcSource instance from mainWindow
	if (auto mw = qobject_cast<MainWindow*>(parent)) {
		tbcSource = &mw->getTbcSource();
	}
	
    // Update the dialogue
    updateDialog();
}

ChromaDecoderConfigDialog::~ChromaDecoderConfigDialog()
{
    delete ui;
}

void ChromaDecoderConfigDialog::setConfiguration(VideoSystem _system, const PalColour::Configuration &_palConfiguration,
                                                 const Comb::Configuration &_ntscConfiguration,
                                                 const MonoDecoder::MonoConfiguration &_monoConfiguration,
												 const TbcSource::SourceMode &_mode,
												 const bool _isInit,
                                                 const OutputWriter::Configuration &_outputConfiguration)
{
    double yNRLevel = _system == NTSC ? ntscConfiguration.yNRLevel : palConfiguration.yNRLevel;
    system = _system;
    palConfiguration = _palConfiguration;
    ntscConfiguration = _ntscConfiguration;
    monoConfiguration = _monoConfiguration;
    outputConfiguration = _outputConfiguration;
	sourceMode = _mode;

    palConfiguration.chromaGain = qBound(0.0, palConfiguration.chromaGain, 2.0);
    palConfiguration.chromaPhase = qBound(-180.0, palConfiguration.chromaPhase, 180.0);
    palConfiguration.transformThreshold = qBound(0.0, palConfiguration.transformThreshold, 1.0);
    palConfiguration.yNRLevel = qBound(0.0, yNRLevel, 10.0);
    ntscConfiguration.cNRLevel = qBound(0.0, ntscConfiguration.cNRLevel, 10.0);
    ntscConfiguration.yNRLevel = qBound(0.0, yNRLevel, 10.0);

    ntscConfiguration.adaptThreshold = qBound(0.1, ntscConfiguration.adaptThreshold, 2.0);
    ntscConfiguration.chromaWeight = qBound(0.0, ntscConfiguration.chromaWeight, 2.0);    

    // For settings that both decoders share, the PAL default takes precedence
    ntscConfiguration.chromaGain = palConfiguration.chromaGain;
    ntscConfiguration.chromaPhase = palConfiguration.chromaPhase;

    // Select the tab corresponding to the current standard automatically
    if (system == NTSC) {
        ui->standardTabs->setCurrentWidget(ui->ntscTab);
    } else {
        ui->standardTabs->setCurrentWidget(ui->palTab);
    }
	
	isInit = _isInit;
    updateDialog();
    emit chromaDecoderConfigChanged();
}

DecodingParameters ChromaDecoderConfigDialog::currentDecodingParameters() const
{
    DecodingParameters parameters;
    if (system == NTSC) {
        if (ntscConfiguration.dimensions == 0) parameters.decoder = "mono";
        else if (ntscConfiguration.dimensions == 1) parameters.decoder = "ntsc1d";
        else if (ntscConfiguration.dimensions == 2) parameters.decoder = "ntsc2d";
        else parameters.decoder = ntscConfiguration.adaptive ? "ntsc3d" : "ntsc3dnoadapt";
        parameters.settings.insert("chromaNR", ntscConfiguration.cNRLevel);
        parameters.settings.insert("lumaNR", ntscConfiguration.yNRLevel);
        parameters.settings.insert("adaptThreshold", ntscConfiguration.adaptThreshold);
        parameters.settings.insert("chromaWeight", ntscConfiguration.chromaWeight);
        parameters.settings.insert("phaseCompensation", ntscConfiguration.phaseCompensation ? 1 : 0);
        parameters.settings.insert("chromaGain", ntscConfiguration.chromaGain);
        parameters.settings.insert("chromaPhase", ntscConfiguration.chromaPhase);
    } else {
        switch (palConfiguration.chromaFilter) {
        case PalColour::mono: parameters.decoder = "mono"; break;
        case PalColour::palColourFilter: parameters.decoder = "pal2d"; break;
        case PalColour::transform2DFilter: parameters.decoder = "transform2d"; break;
        default: parameters.decoder = "transform3d"; break;
        }
        parameters.settings.insert("lumaNR", palConfiguration.yNRLevel);
        parameters.settings.insert("chromaGain", palConfiguration.chromaGain);
        parameters.settings.insert("chromaPhase", palConfiguration.chromaPhase);
        parameters.settings.insert("simplePAL", palConfiguration.simplePAL ? 1 : 0);
    }
    parameters.madeBy = "ld-analyse";
    return parameters;
}

const PalColour::Configuration &ChromaDecoderConfigDialog::getPalConfiguration()
{
    return palConfiguration;
}

const Comb::Configuration &ChromaDecoderConfigDialog::getNtscConfiguration()
{
    return ntscConfiguration;
}

const OutputWriter::Configuration &ChromaDecoderConfigDialog::getOutputConfiguration()
{
    return outputConfiguration;
}

void ChromaDecoderConfigDialog::updateDialog()
{
    const bool isSourcePal = system == PAL || system == PAL_M;
    const bool isSourceNtsc = system == NTSC;
	
	if(!isInit)
	{
		// The decoder the capture was chosen to be decoded with (kept in its metadata), or by the source: one capture in 3D, a stack of sources in 2D; the phase
		// compensation is for a tape (a tape format, or a capture made by vhs-decode)
		const LdDecodeMetaData::VideoParameters &sourceParameters = tbcSource->getVideoParameters();
		const DecodingParameters *stored = tbcSource->getDecodingParameters();
		const QString decoderName = stored ? stored->decoder : DecodingParameters::defaultDecoder(system == NTSC, DecodingParameters::prefers2D(sourceMode != TbcSource::ONE_SOURCE, DecodingParameters::isTape(sourceParameters.tapeFormat, sourceParameters.decoder)));
		if (decoderName == "pal2d") palConfiguration.chromaFilter = PalColour::palColourFilter;
		else if (decoderName == "transform2d") palConfiguration.chromaFilter = PalColour::transform2DFilter;
		else if (decoderName == "transform3d") palConfiguration.chromaFilter = PalColour::transform3DFilter;
		else if (decoderName == "mono") palConfiguration.chromaFilter = PalColour::mono;
		if (decoderName == "ntsc1d") ntscConfiguration.dimensions = 1;
		else if (decoderName == "ntsc2d") ntscConfiguration.dimensions = 2;
		else if (decoderName == "ntsc3d" || decoderName == "ntsc3dnoadapt") { ntscConfiguration.dimensions = 3; ntscConfiguration.adaptive = decoderName == "ntsc3d"; }
		ntscConfiguration.phaseCompensation = DecodingParameters::isTape(sourceParameters.tapeFormat, sourceParameters.decoder);
		if (stored) {
			if (stored->has("chromaGain")) { palConfiguration.chromaGain = stored->value("chromaGain"); ntscConfiguration.chromaGain = stored->value("chromaGain"); }
			if (stored->has("chromaPhase")) { palConfiguration.chromaPhase = stored->value("chromaPhase"); ntscConfiguration.chromaPhase = stored->value("chromaPhase"); }
			if (stored->has("lumaNR")) { palConfiguration.yNRLevel = stored->value("lumaNR"); ntscConfiguration.yNRLevel = stored->value("lumaNR"); monoConfiguration.yNRLevel = stored->value("lumaNR"); }
			if (stored->has("chromaNR")) ntscConfiguration.cNRLevel = stored->value("chromaNR");
			if (stored->has("adaptThreshold")) ntscConfiguration.adaptThreshold = stored->value("adaptThreshold");
			if (stored->has("chromaWeight")) ntscConfiguration.chromaWeight = stored->value("chromaWeight");
			if (stored->has("phaseCompensation")) ntscConfiguration.phaseCompensation = stored->value("phaseCompensation") != 0.0;
			if (stored->has("simplePAL")) palConfiguration.simplePAL = stored->value("simplePAL") != 0.0;
		}
		qInfo().noquote() << "Decoder:" << decoderName << (stored ? "(the decoding parameters kept in the metadata of the capture)"
                                                                   : (sourceMode == TbcSource::ONE_SOURCE ? "(one capture: 3D)" : (DecodingParameters::isTape(sourceParameters.tapeFormat, sourceParameters.decoder) ? "(a stack of tapes: 2D)" : "(a stack of LaserDiscs: 3D)")));
		ui->enableYNRCheckBox->setChecked(yNREnabled);
		ui->enableYCCombineCheckBox->setChecked(combine);
		
		isInit = true;
	}

    // Shared settings

    ui->chromaGainHorizontalSlider->setValue(static_cast<qint32>(palConfiguration.chromaGain * 100));

    ui->chromaGainValueLabel->setEnabled(true);
    ui->chromaGainValueLabel->setText(QString::number(palConfiguration.chromaGain, 'f', 2));

    ui->chromaPhaseHorizontalSlider->setValue(static_cast<qint32>(degreesToSliderPos(palConfiguration.chromaPhase)));

    ui->chromaPhaseValueLabel->setEnabled(true);
    ui->chromaPhaseValueLabel->setText(QString::number(palConfiguration.chromaPhase, 'f', 1) + QChar(0xB0));
    
    ui->yNRHorizontalSlider->setValue(static_cast<qint32>(ynrLevel * 10));
    ui->yNRValueLabel->setText(QString::number(ynrLevel, 'f', 1) + tr(" IRE"));
	
	if(sourceMode == TbcSource::BOTH_SOURCES)
	{
		ui->enableYCCombineCheckBox->show();
	}
	else
	{
		ui->enableYCCombineCheckBox->hide();
	}

    // PAL settings
	
	ui->palMonoRadioButton->setEnabled(isSourcePal);
    ui->palFilterPalColourRadioButton->setEnabled(isSourcePal);
    ui->palFilterTransform2DRadioButton->setEnabled(isSourcePal);
    ui->palFilterTransform3DRadioButton->setEnabled(isSourcePal);
	
	if(isSourcePal)
	{
		switch (palConfiguration.chromaFilter) {
		case PalColour::mono:
			ui->palMonoRadioButton->setChecked(true);
			ui->chromaGainHorizontalSlider->setEnabled(false);
			ui->chromaPhaseHorizontalSlider->setEnabled(false);
			break;
		case PalColour::palColourFilter:
			ui->palFilterPalColourRadioButton->setChecked(true);
			ui->chromaGainHorizontalSlider->setEnabled(true);
			ui->chromaPhaseHorizontalSlider->setEnabled(true);
			break;
		case PalColour::transform2DFilter:
			ui->palFilterTransform2DRadioButton->setChecked(true);
			ui->chromaGainHorizontalSlider->setEnabled(true);
			ui->chromaPhaseHorizontalSlider->setEnabled(true);
			break;
		case PalColour::transform3DFilter:
			ui->palFilterTransform3DRadioButton->setChecked(true);
			ui->chromaGainHorizontalSlider->setEnabled(true);
			ui->chromaPhaseHorizontalSlider->setEnabled(true);
			break;
		}
	}

    const bool isTransform = ((palConfiguration.chromaFilter != PalColour::palColourFilter) && (palConfiguration.chromaFilter != PalColour::mono) );

    ui->thresholdLabel->setEnabled(isSourcePal && isTransform);

    ui->thresholdHorizontalSlider->setEnabled(isSourcePal && isTransform);
    ui->thresholdHorizontalSlider->setValue(static_cast<qint32>(palConfiguration.transformThreshold * 100));

    ui->thresholdValueLabel->setEnabled(isSourcePal && isTransform);
    ui->thresholdValueLabel->setText(QString::number(palConfiguration.transformThreshold, 'f', 2));

    ui->showFFTsCheckBox->setEnabled(isSourcePal && isTransform);
    ui->showFFTsCheckBox->setChecked(palConfiguration.showFFTs);

    ui->simplePALCheckBox->setEnabled(isSourcePal && isTransform);
    ui->simplePALCheckBox->setChecked(palConfiguration.simplePAL);

    // NTSC settings

    ui->phaseCompCheckBox->setEnabled(isSourceNtsc);
    ui->phaseCompCheckBox->setChecked(ntscConfiguration.phaseCompensation);
    ui->ntscMonoRadioButton->setEnabled(isSourceNtsc);
    ui->ntscFilter1DRadioButton->setEnabled(isSourceNtsc);
    ui->ntscFilter2DRadioButton->setEnabled(isSourceNtsc);
    ui->ntscFilter3DRadioButton->setEnabled(isSourceNtsc);
	
	if(isSourceNtsc)
	{
		switch (ntscConfiguration.dimensions) {
		case 0:
			ui->ntscMonoRadioButton->setChecked(true);
			ui->phaseCompCheckBox->setEnabled(false);
			ui->chromaGainHorizontalSlider->setEnabled(false);
			ui->chromaPhaseHorizontalSlider->setEnabled(false);
			break;
		case 1:
			ui->ntscFilter1DRadioButton->setChecked(true);
			ui->phaseCompCheckBox->setEnabled(true);
			ui->chromaGainHorizontalSlider->setEnabled(true);
			ui->chromaPhaseHorizontalSlider->setEnabled(true);
			break;
		case 2:
			ui->ntscFilter2DRadioButton->setChecked(true);
			ui->phaseCompCheckBox->setEnabled(true);
			ui->chromaGainHorizontalSlider->setEnabled(true);
			ui->chromaPhaseHorizontalSlider->setEnabled(true);
			break;
		case 3:
			ui->ntscFilter3DRadioButton->setChecked(true);
			ui->phaseCompCheckBox->setEnabled(true);
			ui->chromaGainHorizontalSlider->setEnabled(true);
			ui->chromaPhaseHorizontalSlider->setEnabled(true);
			break;
		}
    }

    ui->adaptiveCheckBox->setEnabled(isSourceNtsc && ntscConfiguration.dimensions == 3);
    ui->adaptiveCheckBox->setChecked(ntscConfiguration.adaptive);

    ui->showMapCheckBox->setEnabled(isSourceNtsc && ntscConfiguration.dimensions == 3);
    ui->showMapCheckBox->setChecked(ntscConfiguration.showMap);

    ui->adaptThresholdLabel->setEnabled(isSourceNtsc && ntscConfiguration.dimensions == 3);
    ui->adaptThresholdHorizontalSlider->setEnabled(isSourceNtsc && ntscConfiguration.dimensions == 3);
    ui->adaptThresholdHorizontalSlider->setValue(static_cast<qint32>(ntscConfiguration.adaptThreshold * 100));
    ui->adaptThresholdValueLabel->setEnabled(isSourceNtsc && ntscConfiguration.dimensions == 3);
    ui->adaptThresholdValueLabel->setText(QString::number(ntscConfiguration.adaptThreshold, 'f', 2));

    ui->chromaWeightLabel->setEnabled(isSourceNtsc && ntscConfiguration.dimensions == 3);
    ui->chromaWeightHorizontalSlider->setEnabled(isSourceNtsc && ntscConfiguration.dimensions == 3);
    ui->chromaWeightHorizontalSlider->setValue(static_cast<qint32>(ntscConfiguration.chromaWeight * 100));
    ui->chromaWeightValueLabel->setEnabled(isSourceNtsc && ntscConfiguration.dimensions == 3);
    ui->chromaWeightValueLabel->setText(QString::number(ntscConfiguration.chromaWeight, 'f', 2));

    ui->cNRLabel->setEnabled(isSourceNtsc);

    ui->cNRHorizontalSlider->setEnabled(isSourceNtsc && ntscConfiguration.dimensions != 0);
    ui->cNRHorizontalSlider->setValue(static_cast<qint32>(ntscConfiguration.cNRLevel * 10));

    ui->cNRValueLabel->setEnabled(isSourceNtsc);
    ui->cNRValueLabel->setText(QString::number(ntscConfiguration.cNRLevel, 'f', 1) + tr(" IRE"));
}

// Methods to handle changes to the dialogue

void ChromaDecoderConfigDialog::on_chromaGainHorizontalSlider_valueChanged(int value)
{
    palConfiguration.chromaGain = static_cast<double>(value) / 100;
    ntscConfiguration.chromaGain = palConfiguration.chromaGain;
    ui->chromaGainValueLabel->setText(QString::number(palConfiguration.chromaGain, 'f', 2));
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_chromaPhaseHorizontalSlider_valueChanged(int value)
{
    palConfiguration.chromaPhase = sliderPosToDegrees(static_cast<double>(value));
    ntscConfiguration.chromaPhase = palConfiguration.chromaPhase;
    ui->chromaPhaseValueLabel->setText(QString::number(palConfiguration.chromaPhase, 'f', 1) + QChar(0xB0));
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_enableYNRCheckBox_clicked()
{
	yNREnabled = ui->enableYNRCheckBox->isChecked();
	ui->yNRHorizontalSlider->setEnabled(yNREnabled);
	
	if(ui->enableYNRCheckBox->isChecked())
	{
		palConfiguration.yNRLevel = ynrLevel;
		ntscConfiguration.yNRLevel = ynrLevel;
		monoConfiguration.yNRLevel = ynrLevel;
	}
	else
	{
		ynrLevel = monoConfiguration.yNRLevel;
		
		palConfiguration.yNRLevel = static_cast<double>(0);
		ntscConfiguration.yNRLevel = static_cast<double>(0);
		monoConfiguration.yNRLevel = static_cast<double>(0);
	}
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_palFilterButtonGroup_buttonClicked(QAbstractButton *button)
{
	if (button == ui->palMonoRadioButton){
		palConfiguration.chromaFilter = PalColour::mono;
    } else if (button == ui->palFilterPalColourRadioButton) {
        palConfiguration.chromaFilter = PalColour::palColourFilter;
    } else if (button == ui->palFilterTransform2DRadioButton) {
        palConfiguration.chromaFilter = PalColour::transform2DFilter;
    } else {
        palConfiguration.chromaFilter = PalColour::transform3DFilter;
    }
    updateDialog();
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_thresholdHorizontalSlider_valueChanged(int value)
{
    palConfiguration.transformThreshold = static_cast<double>(value) / 100;
    ui->thresholdValueLabel->setText(QString::number(palConfiguration.transformThreshold, 'f', 2));
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_showFFTsCheckBox_clicked()
{
    palConfiguration.showFFTs = ui->showFFTsCheckBox->isChecked();
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_simplePALCheckBox_clicked()
{
    palConfiguration.simplePAL = ui->simplePALCheckBox->isChecked();
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_ntscFilterButtonGroup_buttonClicked(QAbstractButton *button)
{
	if(button == ui->ntscMonoRadioButton){
		ntscConfiguration.dimensions = 0;
	} else if (button == ui->ntscFilter1DRadioButton) {
        ntscConfiguration.dimensions = 1;
    } else if (button == ui->ntscFilter2DRadioButton) {
        ntscConfiguration.dimensions = 2;
    } else {
        ntscConfiguration.dimensions = 3;
    }
    updateDialog();
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_phaseCompCheckBox_clicked()
{
    ntscConfiguration.phaseCompensation = ui->phaseCompCheckBox->isChecked();
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_adaptiveCheckBox_clicked()
{
    ntscConfiguration.adaptive = ui->adaptiveCheckBox->isChecked();
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_showMapCheckBox_clicked()
{
    ntscConfiguration.showMap = ui->showMapCheckBox->isChecked();
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_adaptThresholdHorizontalSlider_valueChanged(int value)
{
    ntscConfiguration.adaptThreshold = static_cast<double>(value) / 100;
    ui->adaptThresholdValueLabel->setText(QString::number(ntscConfiguration.adaptThreshold, 'f', 2));
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_chromaWeightHorizontalSlider_valueChanged(int value)
{
    ntscConfiguration.chromaWeight = static_cast<double>(value) / 100;
    ui->chromaWeightValueLabel->setText(QString::number(ntscConfiguration.chromaWeight, 'f', 2));
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_cNRHorizontalSlider_valueChanged(int value)
{
    ntscConfiguration.cNRLevel = static_cast<double>(value) / 10;
    ui->cNRValueLabel->setText(QString::number(ntscConfiguration.cNRLevel, 'f', 1) + tr(" IRE"));
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_yNRHorizontalSlider_valueChanged(int value)
{
    palConfiguration.yNRLevel = static_cast<double>(value) / 10;
    ntscConfiguration.yNRLevel = static_cast<double>(value) / 10;
	monoConfiguration.yNRLevel = static_cast<double>(value) / 10;
	ynrLevel = static_cast<double>(value) / 10;
    ui->yNRValueLabel->setText(QString::number(ntscConfiguration.yNRLevel, 'f', 1) + tr(" IRE"));
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::updateSourceMode(TbcSource::SourceMode mode)
{
	sourceMode = mode;
	if(sourceMode == TbcSource::BOTH_SOURCES)
	{
		ui->enableYCCombineCheckBox->show();
	}
	else
	{
		ui->enableYCCombineCheckBox->hide();
	}
}

void ChromaDecoderConfigDialog::on_enableYCCombineCheckBox_clicked()
{
	combine = ui->enableYCCombineCheckBox->isChecked();
	tbcSource->setCombine(combine);
	emit chromaDecoderConfigChanged();
}