/******************************************************************************
 * testthemedisabled.cpp
 * ld-decode-tools - a disabled control must look disabled, in every theme and style
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QImage>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStyleFactory>
#include <QVBoxLayout>
#include <cstdlib>
#include <iostream>

#include "theme.h"

// How many pixels of the two renderings differ clearly (by more than a tenth of the range): a control that is dimmed when it is disabled differs by its
// text at least, one that is drawn the same does not differ at all
static int clearlyDifferent(const QImage &a, const QImage &b)
{
    if (a.size() != b.size()) return a.width() * a.height();
    int count = 0;
    for (int y = 0; y < a.height(); y++) {
        for (int x = 0; x < a.width(); x++) {
            if (qAbs(qGray(a.pixel(x, y)) - qGray(b.pixel(x, y))) > 25) count++;
        }
    }
    return count;
}

int main(int argc, char *argv[])
{
    // (offscreen unless the platform is named: THEME_DUMP_DIR with QT_QPA_PLATFORM=windows renders with the real system theme, the windows are not shown)
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    std::cerr << "Testing that a disabled control looks disabled\n";
    int failures = 0;
    int checks = 0;

    const QStringList styles = availableStyles();
    for (const QString &style : styles) {
        for (ThemeMode mode : { ThemeMode::Dark, ThemeMode::DarkGrey, ThemeMode::SoftLight, ThemeMode::Dim }) {
            const bool dark = applyAppearance(style, mode, QString());

            // The boxes and the round buttons have the palette of the program, not one of the platform's (on Windows with a dark system theme the platform fills
            // the ones that are not ticked with the accent colour)
            for (const char *controlClass : { "QCheckBox", "QRadioButton" }) {
                checks++;
                if (QApplication::palette(controlClass).color(QPalette::Base) != QApplication::palette().color(QPalette::Base)
                    || QApplication::palette(controlClass).color(QPalette::Highlight) != QApplication::palette().color(QPalette::Highlight)) {
                    std::cerr << "FAILED: style " << style.toStdString() << ", theme " << static_cast<int>(mode) << ", " << controlClass << " has a palette of its own\n";
                    failures++;
                }
            }

            QWidget window;
            QVBoxLayout layout(&window);
            QLineEdit *edit = new QLineEdit("text");
            QComboBox *combo = new QComboBox;
            combo->addItem("text");
            QSpinBox *spin = new QSpinBox;
            spin->setValue(1234);
            QPushButton *button = new QPushButton("text");
            for (QWidget *widget : QList<QWidget *>({ edit, combo, spin, button })) layout.addWidget(widget);
            // (the indicators too: a box and a radio button, ticked and not, to look at when THEME_DUMP_DIR names a folder for the images)
            QCheckBox *off = new QCheckBox("box, not ticked"), *on = new QCheckBox("box, ticked");
            QRadioButton *radioOff = new QRadioButton("radio, not chosen"), *radioOn = new QRadioButton("radio, chosen");
            on->setChecked(true);
            radioOn->setChecked(true);
            for (QWidget *widget : QList<QWidget *>({ off, on, radioOff, radioOn })) layout.addWidget(widget);
            window.setAttribute(Qt::WA_DontShowOnScreen, true);
            window.show();
            QApplication::processEvents();
            const QString dumpFolder = qEnvironmentVariable("THEME_DUMP_DIR");
            if (!dumpFolder.isEmpty()) {
                const QPalette pal = QApplication::palette();
                std::cerr << style.toStdString() << " theme " << static_cast<int>(mode) << ": window " << pal.color(QPalette::Window).name().toStdString() << " base " << pal.color(QPalette::Base).name().toStdString()
                          << " button " << pal.color(QPalette::Button).name().toStdString() << " highlight " << pal.color(QPalette::Highlight).name().toStdString()
                          << " light " << pal.color(QPalette::Light).name().toStdString() << " mid " << pal.color(QPalette::Mid).name().toStdString() << " dark " << pal.color(QPalette::Dark).name().toStdString() << "\n";
            }
            if (!dumpFolder.isEmpty()) window.grab().save(QDir(dumpFolder).filePath(QString("%1_%2.png").arg(style).arg(static_cast<int>(mode))));

            for (QWidget *widget : QList<QWidget *>({ edit, combo, spin, button })) {
                widget->setEnabled(true);
                QApplication::processEvents();
                const QImage enabled = widget->grab().toImage();
                widget->setEnabled(false);
                QApplication::processEvents();
                const QImage disabled = widget->grab().toImage();
                widget->setEnabled(true);
                checks++;
                const int different = clearlyDifferent(enabled, disabled);
                if (different < 20) {
                    std::cerr << "FAILED: style " << style.toStdString() << ", theme " << static_cast<int>(mode) << (dark ? " (dark)" : " (light)") << ", "
                              << widget->metaObject()->className() << ": only " << different << " pixels differ when it is disabled\n";
                    failures++;
                }
            }
        }
    }
    std::cerr << checks << " checks, " << failures << " failures\n";
    if (failures) return 1;
    std::cout << "testthemedisabled: ok\n";
    return 0;
}
