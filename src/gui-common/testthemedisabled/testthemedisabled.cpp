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
#include <QComboBox>
#include <QImage>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
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
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    std::cerr << "Testing that a disabled control looks disabled\n";
    int failures = 0;
    int checks = 0;

    const QStringList styles = availableStyles();
    for (const QString &style : styles) {
        for (ThemeMode mode : { ThemeMode::Dark, ThemeMode::DarkGrey, ThemeMode::SoftLight, ThemeMode::Dim }) {
            const bool dark = applyAppearance(style, mode, QString());

            QWidget window;
            QVBoxLayout layout(&window);
            QLineEdit *edit = new QLineEdit("text");
            QComboBox *combo = new QComboBox;
            combo->addItem("text");
            QSpinBox *spin = new QSpinBox;
            spin->setValue(1234);
            QPushButton *button = new QPushButton("text");
            for (QWidget *widget : QList<QWidget *>({ edit, combo, spin, button })) layout.addWidget(widget);
            window.show();
            QApplication::processEvents();

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
