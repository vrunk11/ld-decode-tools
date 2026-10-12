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
    setMinimumWidth(520);

    QVBoxLayout *layout = new QVBoxLayout(this);
    QLabel *intro = new QLabel(tr("The video is shown through ffmpeg filters, in this order. They change only the "
                                  "picture shown: the line scope, vectorscope and the other analysis tools still "
                                  "read the TBC data."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    QGridLayout *grid = new QGridLayout;
    grid->setColumnStretch(2, 1);

    deinterlacerCheck = new QCheckBox(tr("Deinterlace:"), this);
    deinterlacerCombo = new QComboBox(this);
    for (const Choice &choice: DEINTERLACERS) deinterlacerCombo->addItem(tr(choice.name), QString::fromLatin1(choice.id));
    grid->addWidget(deinterlacerCheck, 0, 0);
    grid->addWidget(deinterlacerCombo, 0, 1, 1, 2);

    scalerCheck = new QCheckBox(tr("Scale to the display size:"), this);
    scalerCombo = new QComboBox(this);
    for (const Choice &choice: SCALERS) scalerCombo->addItem(tr(choice.name), QString::fromLatin1(choice.id));
    grid->addWidget(scalerCheck, 1, 0);
    grid->addWidget(scalerCombo, 1, 1, 1, 2);

    customCheck = new QCheckBox(tr("Other filters:"), this);
    customEdit = new QLineEdit(this);
    customEdit->setPlaceholderText(tr("e.g. hqdn3d=2:1,unsharp=5:5:0.8"));
    grid->addWidget(customCheck, 2, 0);
    grid->addWidget(customEdit, 2, 1, 1, 2);
    layout->addLayout(grid);

    QLabel *help = new QLabel(tr("Other filters are written as in ffmpeg's -vf option. $W and $H stand for the size "
                                 "the picture is shown at."), this);
    help->setWordWrap(true);
    layout->addWidget(help);

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

    // Anything changed is shown at once in the graph, and the fields follow their check boxes
    const auto update = [this]() {
        deinterlacerCombo->setEnabled(deinterlacerCheck->isChecked());
        scalerCombo->setEnabled(scalerCheck->isChecked());
        customEdit->setEnabled(customCheck->isChecked());
        updateGraphLabel();
    };
    connect(deinterlacerCheck, &QCheckBox::toggled, this, update);
    connect(scalerCheck, &QCheckBox::toggled, this, update);
    connect(customCheck, &QCheckBox::toggled, this, update);
    connect(deinterlacerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, update);
    connect(scalerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, update);
    connect(customEdit, &QLineEdit::textChanged, this, update);
    connect(customEdit, &QLineEdit::returnPressed, this, [this]() { emit settingsChosen(getSettings()); });

    setSettings(VideoFilterSettings());
}

void VideoFilterDialog::setSettings(const VideoFilterSettings &settings)
{
    deinterlacerCheck->setChecked(settings.deinterlacerOn);
    deinterlacerCombo->setCurrentIndex(qMax(0, deinterlacerCombo->findData(settings.deinterlacer)));
    scalerCheck->setChecked(settings.scalerOn);
    scalerCombo->setCurrentIndex(qMax(0, scalerCombo->findData(settings.scaler)));
    customCheck->setChecked(settings.customOn);
    customEdit->setText(settings.custom);

    deinterlacerCombo->setEnabled(settings.deinterlacerOn);
    scalerCombo->setEnabled(settings.scalerOn);
    customEdit->setEnabled(settings.customOn);
    updateGraphLabel();
}

VideoFilterSettings VideoFilterDialog::getSettings() const
{
    VideoFilterSettings settings;
    settings.deinterlacerOn = deinterlacerCheck->isChecked();
    settings.deinterlacer = deinterlacerCombo->currentData().toString();
    settings.scalerOn = scalerCheck->isChecked();
    settings.scaler = scalerCombo->currentData().toString();
    settings.customOn = customCheck->isChecked();
    settings.custom = customEdit->text().trimmed();
    return settings;
}

void VideoFilterDialog::updateGraphLabel()
{
    const QString graph = getSettings().toGraph();
    graphLabel->setText(graph.isEmpty() ? tr("Filter graph: none") : tr("Filter graph: %1").arg(graph));
    resultLabel->clear();
}

void VideoFilterDialog::showResult(const QString &errorText)
{
    if (errorText.isEmpty()) {
        resultLabel->setStyleSheet(QString());
        resultLabel->setText(getSettings().toGraph().isEmpty() ? tr("No filter.") : tr("Filters applied."));
    } else {
        resultLabel->setStyleSheet("color: #d03030;");
        resultLabel->setText(tr("Not applied: %1").arg(errorText));
    }
}
