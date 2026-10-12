/******************************************************************************
 * snrsourcebar.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "snrsourcebar.h"

#include <QHBoxLayout>
#include <QSignalBlocker>

SnrSourceBar::SnrSourceBar(QWidget *parent)
    : QWidget(parent)
{
    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    sourceLabel = new QLabel(this);
    layout->addWidget(sourceLabel, 1);

    measureCheck = new QCheckBox(tr("Measure live"), this);
    measureCheck->setToolTip(tr("Measure the SNR of the video as it arrives. Switched off, what arrives meanwhile is not measured"));
    layout->addWidget(measureCheck);

    followCheck = new QCheckBox(tr("Follow"), this);
    followCheck->setToolTip(tr("Show the whole history as it grows. Zooming or panning the graph stops this, "
                               "so the part looked at stays put"));
    followCheck->setChecked(true);
    layout->addWidget(followCheck);

    connect(measureCheck, &QCheckBox::toggled, this, &SnrSourceBar::measuringToggled);
    setMetadataSource();
}

void SnrSourceBar::setMetadataSource()
{
    sourceLabel->setText(tr("Source: the metadata of the capture (ld-decode)"));
    measureCheck->setVisible(false);
    followCheck->setVisible(false);
}

void SnrSourceBar::setLiveSource(bool measuring, bool wholeHistory)
{
    sourceLabel->setText(wholeHistory ? tr("Source: measured live from the video, every frame received")
                                      : tr("Source: measured live from the video, the frames shown"));
    {
        const QSignalBlocker blocker(measureCheck);
        measureCheck->setChecked(measuring);
    }
    // Without the thread that measures every frame there is nothing to switch off
    measureCheck->setVisible(wholeHistory);
    followCheck->setVisible(true);
}

void SnrSourceBar::setFollowing(bool follow)
{
    followCheck->setChecked(follow);
}
