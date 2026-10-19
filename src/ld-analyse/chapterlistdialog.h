/******************************************************************************
 * chapterlistdialog.h
 * ld-analyse - a small window that lists the chapters of the disc and their times
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef CHAPTERLISTDIALOG_H
#define CHAPTERLISTDIALOG_H

#include <QDialog>
#include <QVector>

class QLabel;
class QPushButton;
class QTableWidget;

// The chapters of the disc as the VBI of the capture gives them, each with the time (or the picture number) of the disc where it begins, the
// frame of the capture and its time; a chapter is chosen and gone to with the button (or a double click)
class ChapterListDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ChapterListDialog(QWidget *parent = nullptr);

    struct Row {
        qint32 chapter = -1;
        qint32 frame = -1;
        qint32 picture = -1;        // the picture number of a CAV disc, -1 if none
        qint32 discSeconds = -1;    // the time of a CLV disc, -1 if none
        qint32 captureSeconds = 0;  // the time in the capture
    };
    // The list to show (none: a message says there are no chapters), and the chapter of a frame is chosen in it
    void setChapters(const QVector<Row> &rows, qint32 currentFrame);
    // The row that is chosen (-1 if none) and how many rows there are
    qint32 currentRow() const;
    qint32 rowCount() const;

signals:
    void goToFrame(qint32 frame);

private:
    void goToCurrent();
    QTableWidget *table;
    QLabel *message;
    QPushButton *goButton;
    QVector<Row> shown;
};

#endif // CHAPTERLISTDIALOG_H
