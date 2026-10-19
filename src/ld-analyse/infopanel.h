/******************************************************************************
 * infopanel.h
 * ld-analyse - the numbers of the disc, shown over the picture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef INFOPANEL_H
#define INFOPANEL_H

#include <QFrame>
#include <QPair>
#include <QString>
#include <QVector>

class QLabel;
class PinTitleBar;

// A window of lines (a name and a value each): the time code, the chapter, the picture number... It floats over the main window (or the
// picture, in the full screen), and has the look of an on-screen display.
class InfoPanel : public QFrame
{
    Q_OBJECT
public:
    explicit InfoPanel(QWidget *parent = nullptr);

    typedef QVector<QPair<QString, QString>> Rows;
    // The lines to show; a line without a name is a heading (the large one at the top: the time code)
    void setRows(const QString &headline, const Rows &rows);
    QString headline() const { return headlineText; }
    Rows rows() const { return shownRows; }

    // Whether it goes away with the other controls when the mouse is still (the pin of its title bar sets it: pinned, it stays, as the
    // screen of a player does)
    bool autoHide() const;
    void setAutoHide(bool on);

signals:
    void closed();
    void autoHideChanged(bool on);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    PinTitleBar *titleBar;
    QLabel *headlineLabel;
    QLabel *body;
    QString headlineText;
    Rows shownRows;
};

#endif // INFOPANEL_H
