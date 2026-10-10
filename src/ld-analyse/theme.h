/******************************************************************************
 * theme.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025 Simon Inns
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef THEME_H
#define THEME_H

#include <QPalette>
#include <QString>
#include <QStringList>
#include <QVector>

// Colour theme of the application, stored in the configuration file (keep the
// values stable). Light and Dark are the system's own light and dark
// appearance (Dark falls back to the Dark grey palette where the system has
// none to offer); Auto follows the system setting. Soft light, Dim and Dark
// grey are fixed neutral-grey palettes (Dark grey is the one ld-analyse used
// for its dark theme); Custom is the palette edited in the theme editor.
enum class ThemeMode {
    Auto = 0,
    Light = 1,
    Dark = 2,
    SoftLight = 3,
    Dim = 4,
    Custom = 5,
    DarkGrey = 6,
};

// A palette colour the theme editor lets the user change
struct ThemeColourRole {
    QPalette::ColorGroup group;
    QPalette::ColorRole role;
    const char *key;     // name stored in the configuration
    const char *label;   // name shown in the editor
};

// The colours the theme editor offers, in display order
const QVector<ThemeColourRole> &themeColourRoles();

// The palette a theme uses (for Custom, the given serialised palette). Light
// and Auto give a light reference palette, as the system one is only known
// once applied.
QPalette themePalette(ThemeMode mode, const QString &customPalette = QString());

// Store/restore the editable colours of a palette as "Key=#rrggbb;..."
QString serializePalette(const QPalette &palette);
QPalette deserializePalette(const QString &text);

// Whether a palette is dark (its window is darker than its text)
bool isDarkPalette(const QPalette &palette);

// Widget styles are Qt style names (as listed by QStyleFactory), stored in the
// configuration file. An empty name means Auto: on Windows the classic style
// ("windowsvista", the Windows 10 look), except on Windows 11 with a dark theme
// where the Windows 11 style draws the system's dark scheme; on other
// platforms the platform's default style. The "windows11" style is only
// offered on Windows 11.

// The styles available on this machine
QStringList availableStyles();
// A readable name for the View > Style menu
QString styleDisplayName(const QString &styleName);
// The classic Windows style, which cannot be dark on its own: with a dark theme
// the "windows" style and a style sheet giving the classic look stand in for it
bool isLightOnlyStyle(const QString &styleName);

// Apply a style (empty for Auto) and a theme to the whole application. It can
// be called again at any time to switch; widgets that cache colours (the
// analysis graphs) must then be refreshed by the caller. If the style was
// given on the command line (Qt's -style option), that style is kept. Returns
// true if the theme is dark.
bool applyAppearance(const QString &styleName, ThemeMode theme, const QString &customPalette = QString());

#endif // THEME_H
