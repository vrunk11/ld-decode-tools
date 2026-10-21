/******************************************************************************
 * testappearancemenus.cpp
 * ld-decode-tools - unit tests for AppearanceMenus
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QApplication>
#include <QMenu>
#include <cstdlib>
#include <iostream>

#include "appearancemenus.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

static QAction *actionOf(QMenu *menu, const QString &title, const QString &text)
{
    for (QAction *action: menu->actions()) {
        if (action->menu() && action->menu()->title() == title) {
            for (QAction *entry: action->menu()->actions()) if (entry->text() == text) return entry;
        }
    }
    return nullptr;
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    std::cerr << "Testing AppearanceMenus\n";

    // A number that is not a theme is Auto; a custom theme without a palette is Auto too: what is stored is not changed
    // (checked through the menu: the Auto entry is the one that is checked)
    for (const auto &[saved, palette, checkedText] : { std::make_tuple(99, QString(), QString("Auto (follow system)")),
                                                        std::make_tuple(5, QString(), QString("Auto (follow system)")),
                                                        std::make_tuple(6, QString(), QString("Dark grey")) }) {
        AppearanceMenus appearance(saved, palette, QString(), nullptr);
        QMenu menu;
        appearance.install(&menu);
        QAction *entry = actionOf(&menu, "Theme", checkedText);
        CHECK(entry != nullptr);
        CHECK(entry->isChecked());
        // Custom is only selectable once a custom theme has been saved
        QAction *custom = actionOf(&menu, "Theme", "Custom");
        CHECK(custom != nullptr);
        CHECK(!custom->isEnabled());
    }

    // Choosing a theme stores it and tells the program that the appearance changed
    {
        AppearanceMenus appearance(0, QString(), QString(), nullptr);
        QMenu menu;
        appearance.install(&menu);
        qint32 storedMode = -1;
        int changes = 0;
        QObject::connect(&appearance, &AppearanceMenus::themeModeStored, [&](qint32 mode) { storedMode = mode; });
        QObject::connect(&appearance, &AppearanceMenus::changed, [&]() { changes++; });
        actionOf(&menu, "Theme", "Dark grey")->trigger();
        CHECK(storedMode == static_cast<qint32>(ThemeMode::DarkGrey));
        CHECK(changes == 1);
        CHECK(appearance.isDark());
        actionOf(&menu, "Theme", "Light")->trigger();
        CHECK(storedMode == static_cast<qint32>(ThemeMode::Light));
        CHECK(changes == 2);
        CHECK(!appearance.isDark());
    }

    // A custom theme with a palette is selectable
    {
        AppearanceMenus appearance(5, serializePalette(themePalette(ThemeMode::DarkGrey)), QString(), nullptr);
        QMenu menu;
        appearance.install(&menu);
        QAction *custom = actionOf(&menu, "Theme", "Custom");
        CHECK(custom->isEnabled());
        CHECK(custom->isChecked());
    }

    std::cerr << "All tests passed\n";
    return 0;
}
