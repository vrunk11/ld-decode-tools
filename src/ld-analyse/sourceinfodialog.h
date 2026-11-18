/******************************************************************************
 * sourceinfodialog.h
 * ld-analyse - a small window with the cover of the file and what is known of the source that is open
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef SOURCEINFODIALOG_H
#define SOURCEINFODIALOG_H

#include <QWidget>

class CoverWidget;
class QFrame;
class QTextBrowser;
class TbcSource;

// The panel of View > Source information, on a side of the window or detached from it: the cover of an LDX file (section 9 of the specification), whole, above what is known of the source: where it is, the video, the
// metadata, and for an LDX file the captures it holds and what was done to it.
class SourceInfoDialog : public QWidget
{
    Q_OBJECT

public:
    explicit SourceInfoDialog(QWidget *parent = nullptr);

    // What is known of the source that is open, and nothing
    void setSource(TbcSource &source);
    void clear();

    // The text of the window for a source (rich text), apart from the widget so that it can be tried
    static QString describe(TbcSource &source);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void fitCover();

    CoverWidget *m_cover = nullptr;
    QFrame *m_frame = nullptr;
    QTextBrowser *m_text = nullptr;
};

#endif // SOURCEINFODIALOG_H
