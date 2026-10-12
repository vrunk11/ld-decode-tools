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

// What the video is shown through: a deinterlacer, a scaling algorithm and any other
// ffmpeg filters, each of which can be switched off without losing its setting. They run in
// that order.
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

    // Show the result of applying the filters (empty for success)
    void showResult(const QString &errorText);

signals:
    // The user asked for these filters (Apply, or OK)
    void settingsChosen(const VideoFilterSettings &settings);

private:
    QCheckBox *deinterlacerCheck;
    QComboBox *deinterlacerCombo;
    QCheckBox *scalerCheck;
    QComboBox *scalerCombo;
    QCheckBox *customCheck;
    QLineEdit *customEdit;
    QLabel *graphLabel;
    QLabel *resultLabel;

    void updateGraphLabel();
};

#endif // VIDEOFILTERDIALOG_H
