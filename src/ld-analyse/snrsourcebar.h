/******************************************************************************
 * snrsourcebar.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef SNRSOURCEBAR_H
#define SNRSOURCEBAR_H

#include <QCheckBox>
#include <QLabel>
#include <QWidget>

// The line above an SNR graph: where the values come from (the metadata of the capture, or measured
// from the video as it arrives), and for a live input a check box to switch the measuring on and
// off and one to have the graph follow the video
class SnrSourceBar : public QWidget
{
    Q_OBJECT
public:
    explicit SnrSourceBar(QWidget *parent = nullptr);

    // The values are those of the metadata (ld-decode's), read from the file
    void setMetadataSource();
    // The values are measured from the video. wholeHistory: for every frame received, rather
    // than only those that were shown
    void setLiveSource(bool measuring, bool wholeHistory);

    bool isFollowing() const { return followCheck->isChecked(); }
    void setFollowing(bool follow);

signals:
    // The user switched the measuring on or off
    void measuringToggled(bool measuring);

private:
    QLabel *sourceLabel;
    QCheckBox *measureCheck;
    QCheckBox *followCheck;
};

#endif // SNRSOURCEBAR_H
