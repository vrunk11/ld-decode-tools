/******************************************************************************
 * chapterlistdialog.cpp
 * ld-analyse - a small window that lists the chapters of the disc and their times
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "chapterlistdialog.h"

#include "remotelogic.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

ChapterListDialog::ChapterListDialog(QWidget *parent) : QDialog(parent, Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint)
{
    setWindowTitle(tr("Chapters"));
    resize(440, 360);
    QVBoxLayout *layout = new QVBoxLayout(this);

    message = new QLabel(tr("This capture has no chapters in its VBI."), this);
    message->setWordWrap(true);
    layout->addWidget(message);

    table = new QTableWidget(0, 5, this);
    table->setHorizontalHeaderLabels({ tr("Chapter"), tr("Disc time"), tr("Picture"), tr("Frame"), tr("Capture time") });
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(table, 1);

    QHBoxLayout *buttons = new QHBoxLayout();
    buttons->addStretch(1);
    goButton = new QPushButton(tr("Go to"), this);
    goButton->setToolTip(tr("Go to the start of the chapter that is chosen (a double click does it too)"));
    goButton->setDefault(true);
    QPushButton *close = new QPushButton(tr("Close"), this);
    buttons->addWidget(goButton);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(goButton, &QPushButton::clicked, this, &ChapterListDialog::goToCurrent);
    connect(table, &QTableWidget::cellDoubleClicked, this, [this]() { goToCurrent(); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    setChapters({}, -1);
}

void ChapterListDialog::setChapters(const QVector<Row> &rows, qint32 currentFrame)
{
    shown = rows;
    table->setRowCount(rows.size());
    qint32 current = -1;
    for (qint32 i = 0; i < rows.size(); i++) {
        const Row &row = rows[i];
        const auto cell = [](const QString &text) {
            QTableWidgetItem *item = new QTableWidgetItem(text);
            item->setTextAlignment(Qt::AlignCenter);
            return item;
        };
        table->setItem(i, 0, cell(QString::number(row.chapter)));
        table->setItem(i, 1, cell(row.discSeconds >= 0 ? clockText(row.discSeconds) : QStringLiteral("-")));
        table->setItem(i, 2, cell(row.picture >= 0 ? QString::number(row.picture) : QStringLiteral("-")));
        table->setItem(i, 3, cell(QString::number(row.frame)));
        table->setItem(i, 4, cell(clockText(row.captureSeconds)));
        // (the chapter that the frame is in: the last one that began at or before it)
        if (currentFrame >= row.frame) current = i;
    }
    message->setVisible(rows.isEmpty());
    table->setVisible(!rows.isEmpty());
    goButton->setEnabled(!rows.isEmpty());
    if (current < 0 && !rows.isEmpty()) current = 0;
    if (current >= 0) table->selectRow(current);
}

qint32 ChapterListDialog::currentRow() const
{
    const QList<QTableWidgetSelectionRange> ranges = table->selectedRanges();
    return ranges.isEmpty() ? -1 : ranges.first().topRow();
}

qint32 ChapterListDialog::rowCount() const
{
    return shown.size();
}

void ChapterListDialog::goToCurrent()
{
    const qint32 row = currentRow();
    if (row >= 0 && row < shown.size()) emit goToFrame(shown[row].frame);
}
