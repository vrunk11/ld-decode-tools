/******************************************************************************
 * appearancemenus.cpp
 * ld-decode-tools - the View > Theme and View > Style menus of the GUI programs
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "appearancemenus.h"

#include <QActionGroup>
#include <QApplication>

#include "themeeditordialog.h"

AppearanceMenus::AppearanceMenus(qint32 savedTheme, const QString &_customPalette, const QString &_styleName, QObject *parent)
    : QObject(parent), customPalette(_customPalette), styleName(_styleName)
{
    themeMode = (savedTheme >= static_cast<qint32>(ThemeMode::Auto) && savedTheme <= static_cast<qint32>(ThemeMode::DarkGrey))
        ? static_cast<ThemeMode>(savedTheme) : ThemeMode::Auto;
    if (themeMode == ThemeMode::Custom && customPalette.isEmpty()) themeMode = ThemeMode::Auto;
    if (qApp->property("forceDarkTheme").toBool()) themeMode = ThemeMode::Dark;

    // A saved style that is not installed here falls back to Auto
    if (!styleName.isEmpty() && !availableStyles().contains(styleName, Qt::CaseInsensitive)) styleName.clear();

    dark = applyAppearance(styleName, themeMode, customPalette);
}

void AppearanceMenus::apply()
{
    dark = applyAppearance(styleName, themeMode, customPalette);
    emit changed();
}

void AppearanceMenus::install(QMenu *menu)
{
    QMenu *themeMenu = new QMenu(tr("Theme"), menu);
    QActionGroup *themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);

    const struct {
        ThemeMode mode;
        QString text;
    } themeEntries[] = {
        // The system's own appearance
        { ThemeMode::Auto, tr("Auto (follow system)") },
        { ThemeMode::Light, tr("Light") },
        { ThemeMode::Dark, tr("Dark") },
        // Fixed palettes
        { ThemeMode::SoftLight, tr("Soft light") },
        { ThemeMode::Dim, tr("Dim") },
        { ThemeMode::DarkGrey, tr("Dark grey") },
        { ThemeMode::Custom, tr("Custom") },
    };

    for (const auto &entry: themeEntries) {
        if (entry.mode == ThemeMode::SoftLight) themeMenu->addSeparator();
        QAction *action = themeMenu->addAction(entry.text);
        action->setCheckable(true);
        action->setChecked(entry.mode == themeMode);
        themeGroup->addAction(action);
        const ThemeMode mode = entry.mode;
        if (mode == ThemeMode::Custom) {
            // Only selectable once a custom theme has been saved
            customThemeAction = action;
            action->setEnabled(!customPalette.isEmpty());
        }
        connect(action, &QAction::triggered, this, [this, mode]() {
            themeMode = mode;
            emit themeModeStored(static_cast<qint32>(mode));
            apply();
        });
    }

    themeMenu->addSeparator();
    QAction *customizeAction = themeMenu->addAction(tr("Customize..."));
    connect(customizeAction, &QAction::triggered, this, [this, menu]() {
        emit customizeRequested(menu->window());
    });

    QMenu *styleMenu = new QMenu(tr("Style"), menu);
    QActionGroup *styleGroup = new QActionGroup(this);
    styleGroup->setExclusive(true);

    // Auto first, then every style Qt finds on this machine
    QStringList names = { QString() };
    names += availableStyles();

    for (const QString &name: names) {
        QString text;
        if (name.isEmpty()) {
#ifdef Q_OS_WIN
            text = tr("Auto (Classic look)");
#else
            text = tr("Auto (system)");
#endif
        } else {
            text = styleDisplayName(name);
        }

        QAction *action = styleMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(name.compare(styleName, Qt::CaseInsensitive) == 0);
        action->setData(name);
        styleGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, name]() {
            styleName = name;
            emit styleNameStored(name);
            apply();
        });
    }

    menu->addMenu(themeMenu);
    menu->addMenu(styleMenu);
}

void AppearanceMenus::customize(QWidget *parent, const QPalette &startPalette)
{
    const ThemeMode previousMode = themeMode;
    const QString previousPalette = customPalette;

    ThemeEditorDialog editor(startPalette, parent);
    connect(&editor, &ThemeEditorDialog::previewChanged, this, [this](const QString &palette) {
        const bool wasDark = dark;
        dark = applyAppearance(styleName, ThemeMode::Custom, palette);
        emit previewed(dark != wasDark);
    });

    if (editor.exec() == QDialog::Accepted) {
        customPalette = editor.palette();
        themeMode = ThemeMode::Custom;
        emit customPaletteStored(customPalette);
        emit themeModeStored(static_cast<qint32>(themeMode));
        if (customThemeAction) {
            customThemeAction->setEnabled(true);
            customThemeAction->setChecked(true);
        }
    } else {
        themeMode = previousMode;
        customPalette = previousPalette;
    }

    apply();
}
