/******************************************************************************
 * testshortcuts.cpp
 * ld-analyse - unit test for the window of keyboard shortcuts
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QApplication>
#include <QKeySequenceEdit>
#include <cstdio>
#include <cstdlib>

#include <QPushButton>
#include <QToolButton>

#include "shortcutsdialog.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            std::fflush(stderr); \
            std::exit(1); \
        } \
    } while (0)

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QAction open("Open"), save("Save"), quit("Quit");
    open.setShortcut(QKeySequence("Ctrl+O"));
    save.setShortcut(QKeySequence("Ctrl+S"));
    QVector<ShortcutCommand> commands;
    commands.append({ "open", "Open", "File", &open, { QKeySequence("Ctrl+O") } });
    commands.append({ "save", "Save", "File", &save, { QKeySequence("Ctrl+S") } });
    commands.append({ "quit", "Quit", "File", &quit, {} });

    ShortcutsDialog dialog(commands);
    dialog.show();
    const QList<QKeySequenceEdit *> editors = dialog.findChildren<QKeySequenceEdit *>();
    CHECK(editors.size() == 3);
    CHECK(dialog.keys()[0] == QList<QKeySequence>({ QKeySequence("Ctrl+O") }));
    CHECK(dialog.keys()[2].isEmpty());

    // A key given to a command that has another's: both say so, and nothing is refused
    editors[2]->setKeySequence(QKeySequence("Ctrl+O"));
    CHECK(editors[0]->toolTip().contains("Quit") && editors[2]->toolTip().contains("Open"));
    CHECK(editors[1]->toolTip().isEmpty());

    // The keys that are changed are given when it is accepted: the new one, and the one that was cleared; not the others
    editors[1]->setKeySequence(QKeySequence());
    QStringList changed;
    QObject::connect(&dialog, &ShortcutsDialog::changed, [&changed](const QString &id, const QList<QKeySequence> &keys) {
        changed << id + "=" + (keys.isEmpty() ? QString() : keys.first().toString(QKeySequence::PortableText));
    });
    dialog.accept();
    // (accept() alone does not give them: the OK button does; here the button is pressed)
    for (QPushButton *button: dialog.findChildren<QPushButton *>()) {
        if (button->text().contains("OK")) button->click();
    }
    changed.sort();
    CHECK(changed == QStringList({ "quit=Ctrl+O", "save=" }));

    // Reset goes back to the keys the command had at the start
    ShortcutsDialog again(commands);
    again.show();
    QKeySequenceEdit *first = again.findChildren<QKeySequenceEdit *>()[0];
    first->setKeySequence(QKeySequence("Ctrl+Q"));
    for (QToolButton *button: again.findChildren<QToolButton *>()) {
        if (button->text() == "Reset") { button->click(); break; }
    }
    CHECK(first->keySequence() == QKeySequence("Ctrl+O"));

    std::fprintf(stderr, "testshortcuts: all tests passed\n");
    return 0;
}
