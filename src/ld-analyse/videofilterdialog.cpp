/******************************************************************************
 * videofilterdialog.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "videofilterdialog.h"

#include <QDialogButtonBox>
#include <QGridLayout>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
struct Choice {
    const char *id;
    const char *name;
    const char *graph;     // for a deinterlacer; a scaler is "scale=$W:$H:flags=<id>"
};

// Each of these gives one picture for each frame (not one for each field)
const Choice DEINTERLACERS[] = {
    { "yadif", QT_TRANSLATE_NOOP("VideoFilterDialog", "yadif"), "yadif=mode=send_frame" },
    { "bwdif", QT_TRANSLATE_NOOP("VideoFilterDialog", "bwdif"), "bwdif=mode=send_frame" },
    { "w3fdif", QT_TRANSLATE_NOOP("VideoFilterDialog", "w3fdif"), "w3fdif=filter=complex:mode=frame" },
    { "estdif", QT_TRANSLATE_NOOP("VideoFilterDialog", "estdif"), "estdif=mode=frame" },
};

const Choice SCALERS[] = {
    { "bilinear", QT_TRANSLATE_NOOP("VideoFilterDialog", "Bilinear"), nullptr },
    { "bicubic", QT_TRANSLATE_NOOP("VideoFilterDialog", "Bicubic"), nullptr },
    { "lanczos", QT_TRANSLATE_NOOP("VideoFilterDialog", "Lanczos"), nullptr },
    { "spline", QT_TRANSLATE_NOOP("VideoFilterDialog", "Spline"), nullptr },
    { "gauss", QT_TRANSLATE_NOOP("VideoFilterDialog", "Gaussian"), nullptr },
    { "area", QT_TRANSLATE_NOOP("VideoFilterDialog", "Area averaging"), nullptr },
    { "neighbor", QT_TRANSLATE_NOOP("VideoFilterDialog", "Nearest neighbour"), nullptr },
};
}

QString VideoFilterSettings::toGraph() const
{
    QStringList filters;
    if (deinterlacerOn) {
        for (const Choice &choice: DEINTERLACERS) {
            if (deinterlacer == QLatin1String(choice.id)) filters << QString::fromLatin1(choice.graph);
        }
    }
    // A scaler gives the picture the size it will be shown at
    if (scalerOn) filters << QString("scale=$W:$H:flags=%1").arg(scaler);
    if (customOn && !custom.trimmed().isEmpty()) filters << custom.trimmed();
    return filters.join(',');
}

bool VideoFilterSettings::operator==(const VideoFilterSettings &other) const
{
    return deinterlacerOn == other.deinterlacerOn && deinterlacer == other.deinterlacer
           && scalerOn == other.scalerOn && scaler == other.scaler
           && customOn == other.customOn && custom == other.custom;
}

VideoFilterDialog::VideoFilterDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Video filter"));
    setMinimumWidth(540);

    QVBoxLayout *layout = new QVBoxLayout(this);
    QLabel *intro = new QLabel(tr("The video is shown through ffmpeg filters. They change only the picture shown: "
                                  "the line scope, vectorscope and the other analysis tools still read the TBC data. "
                                  "Deinterlacing, scaling and the other filters are independent, and run in that order."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    QGridLayout *grid = new QGridLayout;
    grid->setColumnStretch(2, 1);

    // Deinterlacing is the Deinterlaced mode of the view button: only the algorithm is chosen here
    grid->addWidget(new QLabel(tr("Deinterlace with:"), this), 0, 0);
    deinterlacerCombo = new QComboBox(this);
    for (const Choice &choice: DEINTERLACERS) deinterlacerCombo->addItem(tr(choice.name), QString::fromLatin1(choice.id));
    deinterlacerCombo->setToolTip(tr("Used by the Deinterlaced mode of the view button"));
    grid->addWidget(deinterlacerCombo, 0, 1, 1, 2);
    QLabel *deinterlaceHelp = new QLabel(tr("(when the view button is on Deinterlaced)"), this);
    grid->addWidget(deinterlaceHelp, 1, 1, 1, 2);

    scalerCheck = new QCheckBox(tr("Scale to the display size:"), this);
    scalerCombo = new QComboBox(this);
    for (const Choice &choice: SCALERS) scalerCombo->addItem(tr(choice.name), QString::fromLatin1(choice.id));
    grid->addWidget(scalerCheck, 2, 0);
    grid->addWidget(scalerCombo, 2, 1, 1, 2);

    // The command line: always there to edit, and applied when the Filters button is on
    grid->addWidget(new QLabel(tr("Other filters:"), this), 3, 0);
    customEdit = new QLineEdit(this);
    customEdit->setPlaceholderText(tr("e.g. hqdn3d=2:1,unsharp=5:5:0.8"));
    grid->addWidget(customEdit, 3, 1, 1, 2);
    layout->addLayout(grid);

    QLabel *help = new QLabel(tr("The other filters are written as in ffmpeg's -vf option; $W and $H stand for the size "
                                 "the picture is shown at. They are applied when the Filters button is on."), this);
    help->setWordWrap(true);
    layout->addWidget(help);

    buttonStateLabel = new QLabel(this);
    layout->addWidget(buttonStateLabel);

    graphLabel = new QLabel(this);
    graphLabel->setWordWrap(true);
    graphLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(graphLabel);

    resultLabel = new QLabel(this);
    resultLabel->setWordWrap(true);
    layout->addWidget(resultLabel);

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply
                                                     | QDialogButtonBox::Close, this);
    layout->addWidget(buttons);
    connect(buttons->button(QDialogButtonBox::Close), &QPushButton::clicked, this, &QDialog::close);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this]() {
        emit settingsChosen(getSettings());
    });
    connect(buttons->button(QDialogButtonBox::Ok), &QPushButton::clicked, this, [this]() {
        emit settingsChosen(getSettings());
        accept();
    });

    // Anything changed is shown at once in the graph; the scaler's list follows its check box
    const auto update = [this]() {
        scalerCombo->setEnabled(scalerCheck->isChecked());
        updateGraphLabel();
    };
    connect(scalerCheck, &QCheckBox::toggled, this, update);
    connect(deinterlacerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, update);
    connect(scalerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, update);
    connect(customEdit, &QLineEdit::textChanged, this, update);
    connect(customEdit, &QLineEdit::returnPressed, this, [this]() { emit settingsChosen(getSettings()); });

    setSettings(VideoFilterSettings());
    setFiltersOn(true);
}

void VideoFilterDialog::setSettings(const VideoFilterSettings &settings)
{
    deinterlacerCombo->setCurrentIndex(qMax(0, deinterlacerCombo->findData(settings.deinterlacer)));
    scalerCheck->setChecked(settings.scalerOn);
    scalerCombo->setCurrentIndex(qMax(0, scalerCombo->findData(settings.scaler)));
    customEdit->setText(settings.custom);

    scalerCombo->setEnabled(settings.scalerOn);
    updateGraphLabel();
}

// What is chosen here. Whether the deinterlacer and the other filters are on is not: the view
// button (Deinterlaced) and the Filters button say that, so those flags are left to the owner.
VideoFilterSettings VideoFilterDialog::getSettings() const
{
    VideoFilterSettings settings;
    settings.deinterlacerOn = false;
    settings.deinterlacer = deinterlacerCombo->currentData().toString();
    settings.scalerOn = scalerCheck->isChecked();
    settings.scaler = scalerCombo->currentData().toString();
    settings.customOn = true;
    settings.custom = customEdit->text().trimmed();
    return settings;
}

void VideoFilterDialog::setFiltersOn(bool on)
{
    buttonStateLabel->setText(on ? tr("The Filters button is on: the other filters are applied.")
                                 : tr("The Filters button is off: the other filters are not applied."));
}

void VideoFilterDialog::updateGraphLabel()
{
    VideoFilterSettings deinterlacing = getSettings();
    deinterlacing.deinterlacerOn = true;
    deinterlacing.scalerOn = false;
    deinterlacing.customOn = false;
    const QString graph = getSettings().toGraph();
    graphLabel->setText(tr("Deinterlaced view: %1\nScaling and other filters: %2")
                        .arg(deinterlacing.toGraph(), graph.isEmpty() ? tr("none") : graph));
    resultLabel->clear();
}

void VideoFilterDialog::showResult(const QString &errorText)
{
    if (errorText.isEmpty()) {
        resultLabel->setStyleSheet(QString());
        resultLabel->setText(tr("Applied."));
    } else {
        resultLabel->setStyleSheet("color: #d03030;");
        resultLabel->setText(tr("Not applied: %1").arg(errorText));
    }
}
