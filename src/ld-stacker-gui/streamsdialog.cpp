/******************************************************************************
 * streamsdialog.cpp
 * ld-stacker-gui - which other streams of the sources go into the LDX file of the stack
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "streamsdialog.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>

StreamsDialog::StreamsDialog(const StackProject &project, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Streams to copy"));
    resize(560, 360);

    QStringList references;
    QStringList titles;
    for (const StackSource &source: project.sources) {
        if (!source.enabled) continue;
        ids << source.id;
        references << source.reference();
        titles << (source.label.isEmpty() ? QFileInfo(source.file).fileName() : source.label);
    }
    candidates = StreamCopy::candidates(references);
    const QVector<StreamCopy::Candidate> defaults = StreamCopy::defaultChoice(candidates);

    QVBoxLayout *layout = new QVBoxLayout(this);
    QLabel *note = new QLabel(tr("The streams of the sources, besides the video, that are copied as they are into the LDX file of the stack, with the times they have in their source "
                                 "(they are not realigned to the frames of the stack: that is right for the first source when it is not shifted). The history of the file says what was copied. "
                                 "The analogue audio of the first source is written from the fields, and is not one of these."));
    note->setWordWrap(true);
    layout->addWidget(note);

    tree = new QTreeWidget;
    tree->setHeaderHidden(true);
    tree->setRootIsDecorated(true);
    layout->addWidget(tree, 1);

    for (qint32 s = 0; s < ids.size(); s++) {
        QTreeWidgetItem *top = new QTreeWidgetItem(tree, QStringList(tr("Source %1: %2").arg(s + 1).arg(titles[s])));
        top->setExpanded(true);
        qint32 count = 0;
        for (qint32 i = 0; i < candidates.size(); i++) {
            const StreamCopy::Candidate &candidate = candidates[i];
            if (candidate.source != s) continue;
            QTreeWidgetItem *item = new QTreeWidgetItem(top, QStringList(candidate.description()));
            item->setData(0, Qt::UserRole, i);
            if (candidate.derived) {
                item->setFlags(Qt::ItemIsEnabled);
            } else {
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
                bool on = false;
                if (project.stacking.copyStreamsSet) {
                    for (const auto &entry: project.stacking.copyStreams) on = on || (entry.first == ids[s] && entry.second == static_cast<qint32>(candidate.track));
                } else {
                    for (const StreamCopy::Candidate &d: defaults) on = on || (d.source == candidate.source && d.track == candidate.track);
                }
                item->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
            }
            count++;
        }
        if (count == 0) {
            QTreeWidgetItem *none = new QTreeWidgetItem(top, QStringList(tr("no other stream (only an LDX file holds some)")));
            none->setFlags(Qt::NoItemFlags);
        }
    }

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QVector<QPair<QString, qint32>> StreamsDialog::chosen() const
{
    QVector<QPair<QString, qint32>> result;
    for (qint32 s = 0; s < tree->topLevelItemCount(); s++) {
        QTreeWidgetItem *top = tree->topLevelItem(s);
        for (qint32 c = 0; c < top->childCount(); c++) {
            QTreeWidgetItem *item = top->child(c);
            if (!(item->flags() & Qt::ItemIsUserCheckable) || item->checkState(0) != Qt::Checked) continue;
            const StreamCopy::Candidate &candidate = candidates[item->data(0, Qt::UserRole).toInt()];
            result.append({ ids[s], static_cast<qint32>(candidate.track) });
        }
    }
    return result;
}
