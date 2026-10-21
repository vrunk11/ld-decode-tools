/******************************************************************************
 * appearancemenus.h
 * ld-decode-tools - the View > Theme and View > Style menus of the GUI programs
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef APPEARANCEMENUS_H
#define APPEARANCEMENUS_H

#include <QAction>
#include <QMenu>
#include <QObject>
#include <QPalette>
#include <QString>

#include "theme.h"

// The colour theme and widget style of a program, with the menus that choose them. The program keeps the choice where it keeps its
// settings (it listens to the signals "stored") and redraws what keeps colours of its own when the appearance has changed.
class AppearanceMenus : public QObject
{
    Q_OBJECT

public:
    // From what the program has stored (the theme as saved: a number that is not a theme is Auto): applies the appearance at once.
    // --force-dark-theme overrides the theme for the session without changing what is stored.
    AppearanceMenus(qint32 savedTheme, const QString &customPalette, const QString &styleName, QObject *parent = nullptr);

    // Whether the theme applied is dark
    bool isDark() const { return dark; }

    // Add the Theme and Style menus to a menu, with the current choices checked
    void install(QMenu *menu);

    // View > Theme > Customize...: edit the custom theme with a live preview, starting from a palette
    void customize(QWidget *parent, const QPalette &startPalette);

signals:
    // View > Theme > Customize... was chosen: the program calls customize() with the palette to start from
    void customizeRequested(QWidget *parent);
    void themeModeStored(qint32 themeMode);
    void customPaletteStored(const QString &palette);
    void styleNameStored(const QString &styleName);
    // A theme or a style has been chosen and applied
    void changed();
    // The theme editor shows a preview; the argument says whether it went from light to dark or the other way
    void previewed(bool darkChanged);

private:
    void apply();

    ThemeMode themeMode = ThemeMode::Auto;
    QString customPalette;       // custom theme colours (serialised)
    QString styleName;           // empty: Auto
    bool dark = false;
    QAction *customThemeAction = nullptr;
};

#endif // APPEARANCEMENUS_H
