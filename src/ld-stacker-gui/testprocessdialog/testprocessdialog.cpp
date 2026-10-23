/******************************************************************************
 * testprocessdialog.cpp
 * ld-stacker-gui - unit test of the Process window: a stack that is cancelled
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

#include "processdialog.h"
#include "stackproject.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            std::fflush(stderr); \
            std::exit(1); \
        } \
    } while (0)

static QPushButton *buttonNamed(QWidget &parent, const QString &text)
{
    for (QPushButton *button : parent.findChildren<QPushButton *>()) if (button->text() == text) return button;
    return nullptr;
}

static void wait(int milliseconds)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QTemporaryDir directory;
    CHECK(directory.isValid());
    StackProject project;
    for (const char *name : { "a", "b" }) {
        const QString capture = directory.filePath(QString("%1.tbc").arg(name));
        CHECK(QFile::copy(QStringLiteral(TEST_DATA_DIR "/ntsc/ve-snw-cut.tbc"), capture));
        CHECK(QFile::copy(QStringLiteral(TEST_DATA_DIR "/ntsc/ve-snw-cut.tbc.db"), capture + ".db"));
        StackSource source;
        source.id = name;
        source.file = capture;
        project.sources.append(source);
    }

    // Cancelled at once, and then a while after (the program is killed in the middle of what it does, or has just ended): the window says so and does not stop
    for (int delay : { 0, 30, 150 }) {
        const QString output = directory.filePath(QString("stack%1.tbc").arg(delay));
        ProcessDialog dialog(project, output);
        dialog.show();
        QPushButton *start = buttonNamed(dialog, "Start");
        QPushButton *cancel = buttonNamed(dialog, "Cancel");
        CHECK(start && cancel);
        start->click();
        wait(delay);
        if (cancel->isEnabled()) {
            cancel->click();
            QPlainTextEdit *log = dialog.findChild<QPlainTextEdit *>();
            CHECK(log && log->toPlainText().contains("Cancelled"));
            CHECK(!QFileInfo::exists(output));               // (what was started is taken back)
            CHECK(start->isEnabled() && !cancel->isEnabled());
        }
        wait(300);
    }

    std::printf("testprocessdialog: ok\n");
    return 0;
}
