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
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSignalBlocker>
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
    setMinimumWidth(480);

    // A line of explanation under a control, in the colour of placeholder text so it follows the theme
    const auto makeHint = [this](const QString &text) {
        QLabel *hint = new QLabel(text, this);
        hint->setWordWrap(true);
        hint->setForegroundRole(QPalette::PlaceholderText);
        return hint;
    };

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(makeHint(tr("These change only the picture shown, not the data the analysis tools read. "
                                  "They are applied in this order.")));

    // 1. Deinterlacing: only the algorithm is chosen here, the view button switches it on
    QGroupBox *deinterlaceBox = new QGroupBox(tr("1. Deinterlacing"), this);
    QVBoxLayout *deinterlaceLayout = new QVBoxLayout(deinterlaceBox);
    QHBoxLayout *deinterlaceRow = new QHBoxLayout;
    deinterlaceRow->addWidget(new QLabel(tr("Algorithm:"), deinterlaceBox));
    deinterlacerCombo = new QComboBox(deinterlaceBox);
    for (const Choice &choice: DEINTERLACERS) deinterlacerCombo->addItem(tr(choice.name), QString::fromLatin1(choice.id));
    deinterlaceRow->addWidget(deinterlacerCombo, 1);
    deinterlaceLayout->addLayout(deinterlaceRow);
    deinterlaceLayout->addWidget(makeHint(tr("Used when the view button is on Deinterlaced.")));
    layout->addWidget(deinterlaceBox);

    // 2. Scaling
    QGroupBox *scaleBox = new QGroupBox(tr("2. Scaling"), this);
    QVBoxLayout *scaleLayout = new QVBoxLayout(scaleBox);
    QHBoxLayout *scaleRow = new QHBoxLayout;
    scaleRow->addWidget(new QLabel(tr("Algorithm:"), scaleBox));
    scalerCombo = new QComboBox(scaleBox);
    scalerCombo->addItem(tr("Default (fast, smooth)"), QString());
    for (const Choice &choice: SCALERS) scalerCombo->addItem(tr(choice.name), QString::fromLatin1(choice.id));
    scaleRow->addWidget(scalerCombo, 1);
    scaleLayout->addLayout(scaleRow);
    scaleLayout->addWidget(makeHint(tr("The default is the program's own smooth scaling. Any other makes ffmpeg give the picture the size it is shown at, with that algorithm, zooming included.")));
    layout->addWidget(scaleBox);

    // 3. Other filters: a command line, always there to edit; the check box is the Filters button
    QGroupBox *otherBox = new QGroupBox(tr("3. Other filters (ffmpeg -vf)"), this);
    QVBoxLayout *otherLayout = new QVBoxLayout(otherBox);
    customEdit = new QLineEdit(otherBox);
    customEdit->setPlaceholderText(tr("Type ffmpeg filters here (none yet)"));
    otherLayout->addWidget(customEdit);
    filtersCheck = new QCheckBox(tr("Apply them (the Filters button)"), otherBox);
    otherLayout->addWidget(filtersCheck);
    otherLayout->addWidget(makeHint(tr("For example hqdn3d=2:1,unsharp=5:5:0.8. $W and $H are the size the picture is shown at.")));
    layout->addWidget(otherBox);

    // What comes of it: the graph given to ffmpeg, then whether ffmpeg took it
    graphLabel = makeHint(QString());
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

    // Anything changed is shown at once in the graph
    const auto update = [this]() {
        updateGraphLabel();
    };
    connect(deinterlacerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, update);
    connect(scalerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, update);
    connect(customEdit, &QLineEdit::textChanged, this, update);
    connect(customEdit, &QLineEdit::returnPressed, this, [this]() { emit settingsChosen(getSettings()); });
    connect(filtersCheck, &QCheckBox::toggled, this, &VideoFilterDialog::filtersToggled);

    setSettings(VideoFilterSettings());
    setFiltersOn(false);
}

void VideoFilterDialog::setSettings(const VideoFilterSettings &settings)
{
    deinterlacerCombo->setCurrentIndex(qMax(0, deinterlacerCombo->findData(settings.deinterlacer)));
    // No algorithm chosen (the first entry, with no id) is the program's own scaling
    scalerCombo->setCurrentIndex(settings.scalerOn ? qMax(0, scalerCombo->findData(settings.scaler)) : 0);
    customEdit->setText(settings.custom);

    updateGraphLabel();
}

// What is chosen here. Whether the deinterlacer and the other filters are on is not: the view
// button (Deinterlaced) and the Filters button say that, so those flags are left to the owner.
VideoFilterSettings VideoFilterDialog::getSettings() const
{
    VideoFilterSettings settings;
    settings.deinterlacerOn = false;
    settings.deinterlacer = deinterlacerCombo->currentData().toString();
    settings.scaler = scalerCombo->currentData().toString();
    settings.scalerOn = !settings.scaler.isEmpty();
    settings.customOn = true;
    settings.custom = customEdit->text().trimmed();
    return settings;
}

// Show whether the Filters button is on, without it being taken for the user doing it
void VideoFilterDialog::setFiltersOn(bool on)
{
    const QSignalBlocker blocker(filtersCheck);
    filtersCheck->setChecked(on);
}

void VideoFilterDialog::updateGraphLabel()
{
    // What ffmpeg is given: the filters of the Deinterlaced view first, then the scaling and the others
    VideoFilterSettings deinterlacing = getSettings();
    deinterlacing.deinterlacerOn = true;
    deinterlacing.scalerOn = false;
    deinterlacing.customOn = false;
    const QString rest = getSettings().toGraph();
    QStringList parts;
    parts << deinterlacing.toGraph();
    if (!rest.isEmpty()) parts << rest;
    graphLabel->setText(tr("Graph: %1").arg(parts.join(',')));
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
