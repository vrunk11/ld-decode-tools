/******************************************************************************
 * rendererpanel.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef RENDERERPANEL_H
#define RENDERERPANEL_H

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include "renderersettings.h"
#include "vlcsupport.h"

// The choices for a renderer that is not ffmpeg (in Config > Video filter...): where VLC is, the video output and the deinterlacing it is to use, and its options
// (the adjustments of the picture, the sharpening... and the options of the video output that was chosen), all of them as VLC lists them, or the command line of
// another program. What VLC offers is asked of it once, on a thread of its own, and kept.
class RendererPanel : public QWidget
{
    Q_OBJECT
public:
    explicit RendererPanel(QWidget *parent = nullptr);
    ~RendererPanel() override;

    void setSettings(const RendererSettings &settings);
    RendererSettings settings() const;
    // (VLC is asked what it has when the panel is shown for it, not before)
    void showEvent(QShowEvent *event) override;
    // Which of the two pages: VLC (RendererSettings::Vlc) or another program (RendererSettings::Command)
    void setEngine(int engine);

signals:
    void changed();

private:
    struct OptionRow {
        QString module, name;
        bool isInteger = false;
        QCheckBox *set = nullptr;
        QWidget *value = nullptr;
    };

    void loadHelp(bool refresh = false);
    void helpLoaded(const QString &path, bool ok, const QString &error, const VlcSupport::Help &help);
    void rebuildEffects();
    void updateStatus();
    void updatePreview();
    void chooseVlc();
    QString voutValue() const;

    int engine = RendererSettings::Vlc;
    RendererSettings current;           // (the values that the widgets are made from, until they are changed)
    VlcSupport::Help help;
    bool helpReady = false;
    QString helpPath;                   // the VLC that the help is of
    QString failure;                    // why VLC could not be asked (empty if it was)
    bool loading = false;
    bool building = false;

    QWidget *vlcPage = nullptr;
    QWidget *commandPage = nullptr;
    QLineEdit *pathEdit = nullptr;
    QPushButton *reloadButton = nullptr;
    QLabel *statusLabel = nullptr;
    QComboBox *voutCombo = nullptr;
    QComboBox *deinterlaceCombo = nullptr;
    QWidget *effectsWidget = nullptr;
    QVBoxLayout *effectsLayout = nullptr;
    QLineEdit *extraEdit = nullptr;
    QLineEdit *commandEdit = nullptr;
    QLabel *previewLabel = nullptr;
    QVector<OptionRow> rows;
};

#endif // RENDERERPANEL_H
