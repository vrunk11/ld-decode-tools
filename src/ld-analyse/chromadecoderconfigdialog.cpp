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

#include <QTabBar>
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

    // The decoder is chosen in the list, not in the tabs: each page is the settings of one decoder
    ui->standardTabs->tabBar()->hide();

    ui->hvdStrengthHorizontalSlider->setMinimum(0);
    ui->hvdStrengthHorizontalSlider->setMaximum(200);
    
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
                                                 const HvdSettings &_hvdSettings,
                                                 const MonoDecoder::MonoConfiguration &_monoConfiguration,
												 const TbcSource::SourceMode &_mode,
												 const bool _isInit,
                                                 const OutputWriter::Configuration &_outputConfiguration)
{
    double yNRLevel = _system == NTSC ? ntscConfiguration.yNRLevel : palConfiguration.yNRLevel;
    system = _system;
    palConfiguration = _palConfiguration;
    ntscConfiguration = _ntscConfiguration;
    hvdSettings = _hvdSettings;
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
    if (hvdSettings.enabled) {
        parameters = DecodingParameters();
        parameters.decoder = hvdSettings.decoderName();
        parameters.settings.insert("chromaGain", palConfiguration.chromaGain);
        parameters.settings.insert("chromaPhase", palConfiguration.chromaPhase);
        parameters.settings.insert("lumaNR", system == NTSC ? ntscConfiguration.yNRLevel : palConfiguration.yNRLevel);
        parameters.settings.insert("hvdIterations", hvdSettings.iterations);
        parameters.settings.insert("hvdStrength", hvdSettings.strength);
        parameters.settings.insert("hvdOverlap", hvdSettings.overlap);
        parameters.settings.insert("hvdFast", hvdSettings.fast ? 1 : 0);
        parameters.settings.insert("hvdFieldOrder", hvdSettings.fieldOrder);
        parameters.settings.insert("hvdBidirectional", hvdSettings.bidirectional ? 1 : 0);
        parameters.settings.insert("hvdSelective", hvdSettings.selective ? 1 : 0);
        parameters.settings.insert("hvdPasses", hvdSettings.passes);
        parameters.settings.insert("hvdAcc", hvdSettings.acc ? 1 : 0);
        parameters.settings.insert("hvdMonochrome", hvdSettings.monochrome ? 1 : 0);
        parameters.settings.insert("hvdSmoothness", hvdSettings.smoothness);
        parameters.settings.insert("hvdSymmetry", hvdSettings.symmetry ? 1 : 0);
        if (hvdSettings.customSubcarrier) parameters.settings.insert("hvdSubcarrierHz", hvdSettings.subcarrierHz);
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
		palComb = decoderName == "pal2d";
		if (decoderName == "pal2d") palConfiguration.chromaFilter = PalColour::palColourFilter;
		else if (decoderName == "transform2d") palConfiguration.chromaFilter = PalColour::transform2DFilter;
		else if (decoderName == "transform3d") palConfiguration.chromaFilter = PalColour::transform3DFilter;
		else if (decoderName == "mono") palConfiguration.chromaFilter = PalColour::mono;
		if (decoderName == "ntsc1d") ntscConfiguration.dimensions = 1;
		else if (decoderName == "ntsc2d") ntscConfiguration.dimensions = 2;
		else if (decoderName == "ntsc3d" || decoderName == "ntsc3dnoadapt") { ntscConfiguration.dimensions = 3; ntscConfiguration.adaptive = decoderName == "ntsc3d"; }
		ntscConfiguration.phaseCompensation = DecodingParameters::isTape(sourceParameters.tapeFormat, sourceParameters.decoder);
		hvdSettings.enabled = decoderName.startsWith("hvd");
		hvdSettings.temporal = decoderName == "hvd3d";
		if (stored) {
			if (stored->has("chromaGain")) { palConfiguration.chromaGain = stored->value("chromaGain"); ntscConfiguration.chromaGain = stored->value("chromaGain"); }
			if (stored->has("chromaPhase")) { palConfiguration.chromaPhase = stored->value("chromaPhase"); ntscConfiguration.chromaPhase = stored->value("chromaPhase"); }
			if (stored->has("lumaNR")) { palConfiguration.yNRLevel = stored->value("lumaNR"); ntscConfiguration.yNRLevel = stored->value("lumaNR"); monoConfiguration.yNRLevel = stored->value("lumaNR"); }
			if (stored->has("chromaNR")) ntscConfiguration.cNRLevel = stored->value("chromaNR");
			if (stored->has("adaptThreshold")) ntscConfiguration.adaptThreshold = stored->value("adaptThreshold");
			if (stored->has("chromaWeight")) ntscConfiguration.chromaWeight = stored->value("chromaWeight");
			if (stored->has("phaseCompensation")) ntscConfiguration.phaseCompensation = stored->value("phaseCompensation") != 0.0;
			if (stored->has("simplePAL")) palConfiguration.simplePAL = stored->value("simplePAL") != 0.0;
			if (stored->has("hvdIterations")) hvdSettings.iterations = static_cast<qint32>(stored->value("hvdIterations"));
			if (stored->has("hvdStrength")) hvdSettings.strength = stored->value("hvdStrength");
			if (stored->has("hvdOverlap")) hvdSettings.overlap = qMax(1, static_cast<qint32>(stored->value("hvdOverlap")));
			if (stored->has("hvdFast")) hvdSettings.fast = stored->value("hvdFast") != 0.0;
			if (stored->has("hvdFieldOrder")) hvdSettings.fieldOrder = qBound(0, static_cast<qint32>(stored->value("hvdFieldOrder")), 2);
			if (stored->has("hvdBidirectional")) hvdSettings.bidirectional = stored->value("hvdBidirectional") != 0.0;
			if (stored->has("hvdSelective")) hvdSettings.selective = stored->value("hvdSelective") != 0.0;
			if (stored->has("hvdPasses")) hvdSettings.passes = qBound(1, static_cast<qint32>(stored->value("hvdPasses")), 4);
			if (stored->has("hvdAcc")) hvdSettings.acc = stored->value("hvdAcc") != 0.0;
			if (stored->has("hvdMonochrome")) hvdSettings.monochrome = stored->value("hvdMonochrome") != 0.0;
			if (stored->has("hvdSmoothness")) hvdSettings.smoothness = stored->value("hvdSmoothness");
			if (stored->has("hvdSymmetry")) hvdSettings.symmetry = stored->value("hvdSymmetry") != 0.0;
			if (stored->has("hvdSubcarrierHz")) { hvdSettings.customSubcarrier = true; hvdSettings.subcarrierHz = stored->value("hvdSubcarrierHz"); }
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

    // The decoder of the list and the page of its settings
    const bool isSourceHvd = hvdSettings.enabled;
    // (the filter says which one it is, but the mono one, which both have)
    if (palConfiguration.chromaFilter == PalColour::palColourFilter) palComb = true;
    else if (palConfiguration.chromaFilter != PalColour::mono) palComb = false;
    const QString standardName = system == NTSC ? QStringLiteral("NTSC") : (system == PAL_M ? QStringLiteral("PAL-M") : QStringLiteral("PAL"));
    ui->standardLabel->setText(tr("Standard: %1").arg(standardName));
    {
        // PAL: Comb (PALcolour, the decoder with delay lines), Transform and HVD; NTSC: Comb and HVD
        const QSignalBlocker blocker(ui->decoderComboBox);
        ui->decoderComboBox->clear();
        if (isSourcePal) {
            ui->decoderComboBox->addItem(tr("Comb PAL (PALcolour)"), QStringLiteral("comb"));
            ui->decoderComboBox->addItem(tr("Transform PAL"), QStringLiteral("transform"));
            ui->decoderComboBox->addItem(tr("HVD"), QStringLiteral("hvd"));
            ui->decoderComboBox->setCurrentIndex(isSourceHvd ? 2 : (palComb ? 0 : 1));
        } else {
            ui->decoderComboBox->addItem(tr("Comb NTSC"), QStringLiteral("comb"));
            ui->decoderComboBox->addItem(tr("HVD"), QStringLiteral("hvd"));
            ui->decoderComboBox->setCurrentIndex(isSourceHvd ? 1 : 0);
        }
    }
    if (isSourcePal) {
        // The page of PAL is the one of the decoder of the list: Comb has the mono and PALcolour, Transform the mono and its two filters and its settings
        const bool transformPage = !palComb;
        ui->palFilterPalColourRadioButton->setVisible(!transformPage);
        ui->palFilterTransform2DRadioButton->setVisible(transformPage);
        ui->palFilterTransform3DRadioButton->setVisible(transformPage);
        ui->thresholdLabel->setVisible(transformPage);
        ui->thresholdHorizontalSlider->setVisible(transformPage);
        ui->thresholdValueLabel->setVisible(transformPage);
        ui->showFFTsCheckBox->setVisible(transformPage);
        ui->simplePALCheckBox->setVisible(transformPage);
    }
    if (isSourceHvd) {
        ui->standardTabs->setCurrentWidget(ui->hvdTab);
    } else {
        ui->standardTabs->setCurrentWidget(isSourceNtsc ? ui->ntscTab : ui->palTab);
    }

    // HVD settings
    ui->hvd2DRadioButton->setChecked(!hvdSettings.temporal);
    ui->hvd3DRadioButton->setChecked(hvdSettings.temporal);
    {
        const QSignalBlocker iterations(ui->hvdIterationsSpinBox);
        const QSignalBlocker strength(ui->hvdStrengthHorizontalSlider);
        const QSignalBlocker overlap(ui->hvdOverlapSpinBox);
        ui->hvdIterationsSpinBox->setValue(hvdSettings.iterations);
        ui->hvdStrengthHorizontalSlider->setValue(static_cast<qint32>(hvdSettings.strength * 100));
        ui->hvdOverlapSpinBox->setValue(hvdSettings.overlap);
    }
    ui->hvdStrengthValueLabel->setText(hvdSettings.strength == 0.0 ? tr("measured") : QString::number(hvdSettings.strength, 'f', 2));
    ui->hvdStrengthLabel->setEnabled(hvdSettings.temporal);
    ui->hvdStrengthHorizontalSlider->setEnabled(hvdSettings.temporal);
    ui->hvdStrengthValueLabel->setEnabled(hvdSettings.temporal);
    ui->hvdOverlapLabel->setEnabled(hvdSettings.temporal);
    ui->hvdOverlapSpinBox->setEnabled(hvdSettings.temporal);
    {
        const QSignalBlocker fast(ui->hvdFastCheckBox);
        const QSignalBlocker bidirectional(ui->hvdBidirectionalCheckBox);
        const QSignalBlocker selective(ui->hvdSelectiveCheckBox);
        const QSignalBlocker passes(ui->hvdPassesSpinBox);
        const QSignalBlocker fieldOrder(ui->hvdFieldOrderComboBox);
        const QSignalBlocker acc(ui->hvdAccCheckBox);
        const QSignalBlocker monochrome(ui->hvdMonochromeCheckBox);
        const QSignalBlocker smoothness(ui->hvdSmoothnessSpinBox);
        const QSignalBlocker symmetry(ui->hvdSymmetryCheckBox);
        const QSignalBlocker custom(ui->hvdCustomSubcarrierCheckBox);
        const QSignalBlocker subcarrier(ui->hvdSubcarrierSpinBox);
        ui->hvdFastCheckBox->setChecked(hvdSettings.fast);
        ui->hvdBidirectionalCheckBox->setChecked(hvdSettings.bidirectional);
        ui->hvdSelectiveCheckBox->setChecked(hvdSettings.selective);
        ui->hvdPassesSpinBox->setValue(hvdSettings.passes);
        ui->hvdFieldOrderComboBox->setCurrentIndex(hvdSettings.fieldOrder);
        ui->hvdAccCheckBox->setChecked(hvdSettings.acc);
        ui->hvdMonochromeCheckBox->setChecked(hvdSettings.monochrome);
        ui->hvdSmoothnessSpinBox->setValue(hvdSettings.smoothness);
        ui->hvdSymmetryCheckBox->setChecked(hvdSettings.symmetry);
        ui->hvdCustomSubcarrierCheckBox->setChecked(hvdSettings.customSubcarrier);
        ui->hvdSubcarrierSpinBox->setValue(hvdSettings.subcarrierHz);
    }
    // What only 3D uses
    ui->hvdBidirectionalCheckBox->setEnabled(hvdSettings.temporal);
    ui->hvdSelectiveCheckBox->setEnabled(hvdSettings.temporal);
    ui->hvdPassesLabel->setEnabled(hvdSettings.temporal);
    ui->hvdPassesSpinBox->setEnabled(hvdSettings.temporal);
    ui->hvdSubcarrierSpinBox->setEnabled(hvdSettings.customSubcarrier);

    if (isSourceHvd) {
        // The gain and the phase are the decoder's too, the luma noise reduction is not
        ui->chromaGainHorizontalSlider->setEnabled(true);
        ui->chromaPhaseHorizontalSlider->setEnabled(true);
    }
    ui->enableYNRCheckBox->setEnabled(true);
    ui->yNRHorizontalSlider->setEnabled(yNREnabled);
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
        palComb = true;
        palConfiguration.chromaFilter = PalColour::palColourFilter;
    } else if (button == ui->palFilterTransform2DRadioButton) {
        palComb = false;
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
const HvdSettings &ChromaDecoderConfigDialog::getHvdSettings()
{
    return hvdSettings;
}

void ChromaDecoderConfigDialog::on_decoderComboBox_activated(int index)
{
    const QString chosen = ui->decoderComboBox->itemData(index).toString();
    hvdSettings.enabled = chosen == QLatin1String("hvd");
    if (system != NTSC && !hvdSettings.enabled) {
        palComb = chosen == QLatin1String("comb");
        // The filter that goes with the decoder (the mono one belongs to both)
        if (palComb && palConfiguration.chromaFilter != PalColour::mono) palConfiguration.chromaFilter = PalColour::palColourFilter;
        if (!palComb && palConfiguration.chromaFilter == PalColour::palColourFilter) palConfiguration.chromaFilter = PalColour::transform3DFilter;
    }
    updateDialog();
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_hvdFilterButtonGroup_buttonClicked(QAbstractButton *button)
{
    hvdSettings.temporal = button == ui->hvd3DRadioButton;
    updateDialog();
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_hvdIterationsSpinBox_valueChanged(int value)
{
    hvdSettings.iterations = value;
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_hvdStrengthHorizontalSlider_valueChanged(int value)
{
    hvdSettings.strength = static_cast<double>(value) / 100;
    ui->hvdStrengthValueLabel->setText(hvdSettings.strength == 0.0 ? tr("measured") : QString::number(hvdSettings.strength, 'f', 2));
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_hvdOverlapSpinBox_valueChanged(int value)
{
    hvdSettings.overlap = value;
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_hvdFastCheckBox_clicked()
{
    hvdSettings.fast = ui->hvdFastCheckBox->isChecked();
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_hvdBidirectionalCheckBox_clicked()
{
    hvdSettings.bidirectional = ui->hvdBidirectionalCheckBox->isChecked();
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_hvdSelectiveCheckBox_clicked()
{
    hvdSettings.selective = ui->hvdSelectiveCheckBox->isChecked();
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_hvdPassesSpinBox_valueChanged(int value)
{
    hvdSettings.passes = value;
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_hvdFieldOrderComboBox_activated(int index)
{
    hvdSettings.fieldOrder = index;
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_hvdAccCheckBox_clicked()
{
    hvdSettings.acc = ui->hvdAccCheckBox->isChecked();
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_hvdMonochromeCheckBox_clicked()
{
    hvdSettings.monochrome = ui->hvdMonochromeCheckBox->isChecked();
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_hvdSmoothnessSpinBox_valueChanged(double value)
{
    hvdSettings.smoothness = value;
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_hvdSymmetryCheckBox_clicked()
{
    hvdSettings.symmetry = ui->hvdSymmetryCheckBox->isChecked();
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_hvdCustomSubcarrierCheckBox_clicked()
{
    hvdSettings.customSubcarrier = ui->hvdCustomSubcarrierCheckBox->isChecked();
    ui->hvdSubcarrierSpinBox->setEnabled(hvdSettings.customSubcarrier);
    emit chromaDecoderConfigChanged();
}

void ChromaDecoderConfigDialog::on_hvdSubcarrierSpinBox_valueChanged(double value)
{
    hvdSettings.subcarrierHz = value;
    emit chromaDecoderConfigChanged();
}
