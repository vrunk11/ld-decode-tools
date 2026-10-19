/******************************************************************************
 * shortcutsdialog.cpp
 * ld-analyse - the keyboard shortcuts of the program, with the keys that can be changed
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "shortcutsdialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

static QString keysText(const QList<QKeySequence> &keys)
{
    QStringList parts;
    for (const QKeySequence &key: keys) parts << key.toString(QKeySequence::NativeText);
    return parts.join(", ");
}

ShortcutsDialog::ShortcutsDialog(const QVector<ShortcutCommand> &commands, QWidget *parent) : QDialog(parent), commandList(commands)
{
    setWindowTitle(tr("Keyboard shortcuts"));
    resize(720, 560);
    QVBoxLayout *layout = new QVBoxLayout(this);

    QHBoxLayout *top = new QHBoxLayout();
    top->addWidget(new QLabel(tr("Search"), this));
    filter = new QLineEdit(this);
    filter->setPlaceholderText(tr("A command, a menu or a key"));
    filter->setClearButtonEnabled(true);
    top->addWidget(filter, 1);
    QPushButton *resetAllButton = new QPushButton(tr("Reset all"), this);
    top->addWidget(resetAllButton);
    layout->addLayout(top);

    table = new QTableWidget(commandList.size(), 4, this);
    table->setHorizontalHeaderLabels({ tr("Menu"), tr("Command"), tr("Shortcut"), tr("Default") });
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->setColumnWidth(2, 190);
    layout->addWidget(table, 1);

    QLabel *help = new QLabel(tr("Click a shortcut and press the keys to have. Backspace clears it. A key that two commands have is in red. "
                                  "The keys that were changed are kept for the next time."), this);
    help->setWordWrap(true);
    layout->addWidget(help);

    editors.resize(commandList.size());
    for (qint32 row = 0; row < commandList.size(); row++) {
        const ShortcutCommand &command = commandList[row];
        table->setItem(row, 0, new QTableWidgetItem(command.menu));
        table->setItem(row, 1, new QTableWidgetItem(command.text));
        table->setItem(row, 3, new QTableWidgetItem(keysText(command.defaults)));
        QWidget *cell = new QWidget(table);
        QHBoxLayout *cellLayout = new QHBoxLayout(cell);
        cellLayout->setContentsMargins(2, 0, 2, 0);
        QKeySequenceEdit *edit = new QKeySequenceEdit(command.action ? command.action->shortcut() : QKeySequence(), cell);
        edit->setMaximumSequenceLength(1);
        edit->setClearButtonEnabled(true);
        QToolButton *reset = new QToolButton(cell);
        reset->setText(tr("Reset"));
        reset->setToolTip(tr("The key this command had at the start"));
        cellLayout->addWidget(edit, 1);
        cellLayout->addWidget(reset);
        table->setCellWidget(row, 2, cell);
        editors[row] = edit;
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this, [this]() { rowChanged(); });
        connect(reset, &QToolButton::clicked, this, [this, row]() { resetRow(row); });
        table->setRowHeight(row, qMax(28, edit->sizeHint().height() + 4));
    }

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        for (qint32 row = 0; row < commandList.size(); row++) {
            const QKeySequence now = editors[row]->keySequence();
            QList<QKeySequence> wanted;
            if (!now.isEmpty()) wanted << now;
            const QList<QKeySequence> current = commandList[row].action ? commandList[row].action->shortcuts() : QList<QKeySequence>();
            if (wanted != current) emit changed(commandList[row].id, wanted);
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(resetAllButton, &QPushButton::clicked, this, &ShortcutsDialog::resetAll);
    connect(filter, &QLineEdit::textChanged, this, &ShortcutsDialog::applyFilter);
    checkClashes();
}

QVector<QList<QKeySequence>> ShortcutsDialog::keys() const
{
    QVector<QList<QKeySequence>> all;
    for (QKeySequenceEdit *edit: editors) {
        QList<QKeySequence> one;
        if (!edit->keySequence().isEmpty()) one << edit->keySequence();
        all.append(one);
    }
    return all;
}

void ShortcutsDialog::rowChanged()
{
    checkClashes();
}

void ShortcutsDialog::resetRow(int row)
{
    const QList<QKeySequence> &defaults = commandList[row].defaults;
    editors[row]->setKeySequence(defaults.isEmpty() ? QKeySequence() : defaults.first());
}

void ShortcutsDialog::resetAll()
{
    for (qint32 row = 0; row < commandList.size(); row++) resetRow(row);
}

void ShortcutsDialog::applyFilter(const QString &text)
{
    for (qint32 row = 0; row < commandList.size(); row++) {
        const QString haystack = commandList[row].menu + " " + commandList[row].text + " " + editors[row]->keySequence().toString(QKeySequence::NativeText)
                                 + " " + keysText(commandList[row].defaults);
        table->setRowHidden(row, !text.isEmpty() && !haystack.contains(text, Qt::CaseInsensitive));
    }
}

// A key that two commands have: both rows in red, and each says who else has it
void ShortcutsDialog::checkClashes()
{
    QMap<QString, QVector<qint32>> byKey;
    for (qint32 row = 0; row < commandList.size(); row++) {
        const QKeySequence key = editors[row]->keySequence();
        if (!key.isEmpty()) byKey[key.toString(QKeySequence::PortableText)].append(row);
    }
    for (qint32 row = 0; row < commandList.size(); row++) {
        const QKeySequence key = editors[row]->keySequence();
        QStringList others;
        if (!key.isEmpty()) {
            for (qint32 other: byKey.value(key.toString(QKeySequence::PortableText))) {
                if (other != row) others << commandList[other].text;
            }
        }
        const bool clash = !others.isEmpty();
        for (qint32 column: { 0, 1, 3 }) {
            QTableWidgetItem *item = table->item(row, column);
            item->setForeground(clash ? QBrush(QColor(220, 50, 50)) : QBrush());
            item->setToolTip(clash ? tr("The same key as: %1").arg(others.join(", ")) : QString());
        }
        editors[row]->setStyleSheet(clash ? "QKeySequenceEdit { color: #dc3232; }" : QString());
        editors[row]->setToolTip(clash ? tr("The same key as: %1").arg(others.join(", ")) : QString());
    }
}
