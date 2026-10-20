/******************************************************************************
 * sourceinfopanel.h
 * ld-stacker-gui - the information on the source that is chosen
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef SOURCEINFOPANEL_H
#define SOURCEINFOPANEL_H

#include <QFrame>

struct SourceInfo;
class QLabel;
class QToolButton;
class QTextBrowser;

// A panel between the list of the sources and the picture, with what is known of the source that is chosen and that the window does not show
// otherwise: where it is and how it is kept, the video, what the VBI says of the disc (and where the numbering jumps), whether it was mapped, the
// SNR of its metadata, and the history of an LDX file.
class SourceInfoPanel : public QFrame
{
    Q_OBJECT
public:
    explicit SourceInfoPanel(QWidget *parent = nullptr);

    // The source to show (not loaded: what is known of it, with its error); a message instead when there is none
    void setSource(const SourceInfo &info, const QString &title, bool loaded);
    void setMessage(const QString &message);
    // Whether the source can be mapped from here (a TBC capture that is not mapped), and why not
    void setMappable(bool mappable, const QString &why);

    // The text of the panel for a source (rich text), apart from the widget so that it can be tried
    static QString describe(const SourceInfo &info, const QString &title, bool loaded);

signals:
    void mapRequested();

private:
    QTextBrowser *browser;
    QToolButton *mapButton;
    QLabel *mapStatus;
};

#endif // SOURCEINFOPANEL_H
