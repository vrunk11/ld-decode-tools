/******************************************************************************
 * shortcutsdialog.h
 * ld-analyse - the keyboard shortcuts of the program, with the keys that can be changed
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef SHORTCUTSDIALOG_H
#define SHORTCUTSDIALOG_H

#include <QAction>
#include <QDialog>
#include <QKeySequence>
#include <QList>
#include <QVector>

class QKeySequenceEdit;
class QLineEdit;
class QTableWidget;

// A command of the program that has keys: the action that does it, what it is called and where it is in the menus, and the keys it had
// when the program began (which "Reset" goes back to)
struct ShortcutCommand {
    QString id;                         // what is kept in the configuration
    QString text;
    QString menu;
    QAction *action = nullptr;
    QList<QKeySequence> defaults;
};

// Keyboard shortcuts: every command that has a menu entry or a key, with its keys, which can be changed. A key that two commands have
// is shown in red (the commands that have it are named). OK gives the keys that were changed with `changed`.
class ShortcutsDialog : public QDialog
{
    Q_OBJECT
public:
    ShortcutsDialog(const QVector<ShortcutCommand> &commands, QWidget *parent = nullptr);

    // The keys of each command, as they stand in the window (for the check of the keys that clash, and for what is shown)
    QVector<QList<QKeySequence>> keys() const;

signals:
    void changed(const QString &id, const QList<QKeySequence> &keys);

private:
    void rowChanged();
    void resetRow(int row);
    void resetAll();
    void applyFilter(const QString &text);
    void checkClashes();

    QVector<ShortcutCommand> commandList;
    QTableWidget *table;
    QVector<QKeySequenceEdit *> editors;
    QLineEdit *filter;
};

#endif // SHORTCUTSDIALOG_H
