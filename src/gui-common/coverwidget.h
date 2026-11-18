/******************************************************************************
 * coverwidget.h
 * ld-decode-tools - the cover of an LDX file, as the programs show it above what they know of the file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef COVERWIDGET_H
#define COVERWIDGET_H

#include <QByteArray>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;

// The cover of an LDX file (section 9 of the specification): the picture whole, in a frame that fits its shape, which opens the link of the cover (link.url) when it
// is clicked. The picture is the one in the file; or, when there is none, the one the file points to (cover.url): a file, whose path can be relative to the folder
// of the LDX file, or a web address, which is **fetched only when the user asks** (a file that is given to someone must not make their computer ask a server
// for anything by itself). A widget that has nothing to show is hidden.
class CoverWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CoverWidget(QWidget *parent = nullptr);

    // What the file keeps (see LdxCapture::CoverInfo); ldxPath is the file the relative path of the picture is relative to
    void setContent(const QByteArray &image, const QString &source, const QString &link, const QString &ldxPath);
    // The room the picture is fitted to (its width is also what the widget fits to, up to the width it has)
    void setRoom(const QSize &room);
    // What the widget shows now
    bool hasPicture() const { return !m_picture.isNull(); }
    QSize pictureSize() const { return m_picture.size(); }
    // Scale the picture up as well, to fill the room (by default it is only scaled down)
    void setFill(bool fill) { m_fill = fill; fit(); }
    QString link() const { return m_link; }

    // Whether this build can fetch a web address
    static bool webAvailable();

signals:
    // The picture was clicked (the link is opened by the widget)
    void clicked();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void fit();
    void fetch();
    void show(const QPixmap &picture, const QString &text);

    QLabel *m_image = nullptr;
    QLabel *m_text = nullptr;
    QPushButton *m_fetch = nullptr;
    QPixmap m_picture;
    QString m_link;
    QString m_source;
    QSize m_room { 400, 240 };
    bool m_fill = false;
};

#endif // COVERWIDGET_H
