/******************************************************************************
 * videofilterdialog.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef VIDEOFILTERDIALOG_H
#define VIDEOFILTERDIALOG_H

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>

#include "rendererpanel.h"
#include "renderersettings.h"

// What the video is shown through: a deinterlacer, a scaling algorithm and any other ffmpeg
// filters, which run in that order. The deinterlacer is used by the Deinterlaced mode of the
// view button and the other filters when the Filters button is on, so those two flags are set by
// the window; the scaler has its own switch.
struct VideoFilterSettings {
    bool deinterlacerOn = false;
    QString deinterlacer = QStringLiteral("yadif");
    bool scalerOn = false;
    QString scaler = QStringLiteral("lanczos");
    bool customOn = false;
    QString custom;

    // The ffmpeg filter graph for the filters that are on (empty if none is)
    QString toGraph() const;
    bool operator==(const VideoFilterSettings &other) const;
};

// Choose the video filters (View > Video filter...). The dialogue only holds the choice;
// the owner applies it and says whether ffmpeg accepted it.
class VideoFilterDialog : public QDialog
{
    Q_OBJECT
public:
    explicit VideoFilterDialog(QWidget *parent = nullptr);

    void setSettings(const VideoFilterSettings &settings);
    VideoFilterSettings getSettings() const;

    // What shows the picture: ffmpeg's filters (the rest of this dialogue), VLC, or another program
    RendererSettings rendererSettings() const;
    void setRendererSettings(const RendererSettings &settings);

    // Show whether the Filters button is on (it applies the other filters)
    void setFiltersOn(bool on);

    // Show the result of applying the filters (empty for success)
    void showResult(const QString &errorText);

signals:
    // The user chose another renderer, or changed an option of one (VLC is started again with it)
    void rendererChosen(const RendererSettings &settings);
    // The user ticked or unticked Apply them: the Filters button is to be switched
    void filtersToggled(bool on);
    // The user asked for these filters (Apply, or OK)
    void settingsChosen(const VideoFilterSettings &settings);

private:
    QComboBox *engineCombo;
    RendererPanel *rendererPanel;
    QWidget *ffmpegPage;
    QComboBox *deinterlacerCombo;
    QComboBox *scalerCombo;
    QLineEdit *customEdit;
    QCheckBox *filtersCheck;
    QLabel *graphLabel;
    QLabel *resultLabel;

    void updateGraphLabel();
};

#endif // VIDEOFILTERDIALOG_H
