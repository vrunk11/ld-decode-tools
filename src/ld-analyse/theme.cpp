/******************************************************************************
 * theme.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025 Simon Inns
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QOperatingSystemVersion>
#include <QStyleFactory>
#include <QStyleHints>

#ifdef Q_OS_WIN
#include <QSettings>
#elif defined(Q_OS_MACOS)
#include <QProcess>
#elif defined(Q_OS_LINUX)
#include <QProcess>
#endif

// Cross-platform function to detect if system is in dark mode
static bool isDarkModeEnabled() {
#ifdef Q_OS_WIN
    QSettings settings("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", QSettings::NativeFormat);
    return settings.value("AppsUseLightTheme", 1).toInt() == 0;
#elif defined(Q_OS_MACOS)
    QProcess process;
    process.start("defaults", QStringList() << "read" << "-g" << "AppleInterfaceStyle");
    process.waitForFinished();
    QString result = process.readAllStandardOutput().trimmed();
    return result == "Dark";
#elif defined(Q_OS_LINUX)
    // Try gsettings for GNOME color-scheme (modern approach)
    QProcess process;
    process.start("gsettings", QStringList() << "get" << "org.gnome.desktop.interface" << "color-scheme");
    process.waitForFinished();
    QString result = process.readAllStandardOutput().trimmed();
    result = result.remove('\'').remove('"'); // Remove quotes
    if (result.contains("dark", Qt::CaseInsensitive)) {
        return true;
    }

    // Fallback: Check GTK theme name
    process.start("gsettings", QStringList() << "get" << "org.gnome.desktop.interface" << "gtk-theme");
    process.waitForFinished();
    result = process.readAllStandardOutput().trimmed();
    result = result.remove('\'').remove('"');
    return result.contains("dark", Qt::CaseInsensitive);
#endif
    return false;
}

// The colours the theme editor offers. QPalette::All sets every colour group;
// the "Disabled text" entry (Disabled/WindowText) also sets the disabled Text
// and ButtonText, so that greyed-out controls are readable in any theme.
const QVector<ThemeColourRole> &themeColourRoles()
{
    static const QVector<ThemeColourRole> roles = {
        { QPalette::All, QPalette::Window, "Window", "Window background" },
        { QPalette::All, QPalette::WindowText, "WindowText", "Window text" },
        { QPalette::All, QPalette::Base, "Base", "Field background" },
        { QPalette::All, QPalette::AlternateBase, "AlternateBase", "Alternate field background" },
        { QPalette::All, QPalette::Text, "Text", "Field text" },
        { QPalette::All, QPalette::Button, "Button", "Button" },
        { QPalette::All, QPalette::ButtonText, "ButtonText", "Button text" },
        { QPalette::All, QPalette::Highlight, "Highlight", "Selection" },
        { QPalette::All, QPalette::HighlightedText, "HighlightedText", "Selected text" },
        { QPalette::All, QPalette::ToolTipBase, "ToolTipBase", "Tooltip background" },
        { QPalette::All, QPalette::ToolTipText, "ToolTipText", "Tooltip text" },
        { QPalette::All, QPalette::PlaceholderText, "PlaceholderText", "Placeholder text" },
        { QPalette::All, QPalette::Link, "Link", "Link" },
        { QPalette::Disabled, QPalette::WindowText, "DisabledText", "Disabled text" },
    };
    return roles;
}

static void setThemeColour(QPalette &palette, const ThemeColourRole &entry, const QColor &colour)
{
    palette.setColor(entry.group, entry.role, colour);
    if (entry.group == QPalette::Disabled && entry.role == QPalette::WindowText) {
        palette.setColor(QPalette::Disabled, QPalette::Text, colour);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, colour);
    }
}

static QColor themeColour(const QPalette &palette, const ThemeColourRole &entry)
{
    return palette.color(entry.group == QPalette::All ? QPalette::Active : entry.group, entry.role);
}

// Build a palette from "key" -> colour pairs; roles not listed are left unset
// and so come from the system palette when applied
static QPalette makePalette(std::initializer_list<std::pair<const char *, const char *>> colours)
{
    QPalette palette;
    for (const auto &colour : colours) {
        for (const ThemeColourRole &entry : themeColourRoles()) {
            if (qstrcmp(entry.key, colour.first) == 0) setThemeColour(palette, entry, QColor(colour.second));
        }
    }
    return palette;
}

// The dark theme palette (colours as originally chosen for ld-analyse)
static QPalette darkPalette()
{
    return makePalette({
        { "Window", "#353535" }, { "WindowText", "#ffffff" },
        { "Base", "#191919" }, { "AlternateBase", "#404040" }, { "Text", "#ffffff" },
        { "Button", "#353535" }, { "ButtonText", "#ffffff" },
        { "Highlight", "#2a82da" }, { "HighlightedText", "#ffffff" },
    });
}

// Neutral light grey, lower contrast than the system's white
static QPalette softLightPalette()
{
    return makePalette({
        { "Window", "#e1e1e1" }, { "WindowText", "#202020" },
        { "Base", "#ececec" }, { "AlternateBase", "#e1e1e1" }, { "Text", "#202020" },
        { "Button", "#d6d6d6" }, { "ButtonText", "#202020" },
        { "Highlight", "#4a7ab0" }, { "HighlightedText", "#ffffff" },
        { "ToolTipBase", "#f0f0f0" }, { "ToolTipText", "#202020" },
        { "PlaceholderText", "#707070" }, { "Link", "#2a5d9f" },
        { "DisabledText", "#8a8a8a" },
    });
}

// Neutral mid-dark grey, softer than the dark theme
static QPalette dimPalette()
{
    return makePalette({
        { "Window", "#404040" }, { "WindowText", "#dcdcdc" },
        { "Base", "#333333" }, { "AlternateBase", "#3a3a3a" }, { "Text", "#dcdcdc" },
        { "Button", "#4a4a4a" }, { "ButtonText", "#dcdcdc" },
        { "Highlight", "#4a6f9a" }, { "HighlightedText", "#ffffff" },
        { "ToolTipBase", "#4a4a4a" }, { "ToolTipText", "#dcdcdc" },
        { "PlaceholderText", "#8c8c8c" }, { "Link", "#7aa6d8" },
        { "DisabledText", "#7a7a7a" },
    });
}

// A light palette to show for Light/Auto when the system one is not applied:
// Fusion's standard palette is a plain light grey (QStyle's default on Windows
// is the old grey-beige Windows 2000 one)
static QPalette lightReferencePalette()
{
    QStyle *fusion = QStyleFactory::create("fusion");
    if (!fusion) return QApplication::palette();
    const QPalette palette = fusion->standardPalette();
    delete fusion;
    return palette;
}

QPalette themePalette(ThemeMode mode, const QString &customPalette)
{
    switch (mode) {
    case ThemeMode::Dark:
    case ThemeMode::DarkGrey: return darkPalette();
    case ThemeMode::SoftLight: return softLightPalette();
    case ThemeMode::Dim: return dimPalette();
    case ThemeMode::Custom: return deserializePalette(customPalette);
    case ThemeMode::Auto:
    case ThemeMode::Light:
        break;
    }
    return lightReferencePalette();
}

QString serializePalette(const QPalette &palette)
{
    QStringList entries;
    for (const ThemeColourRole &entry : themeColourRoles()) {
        entries << QString("%1=%2").arg(entry.key, themeColour(palette, entry).name());
    }
    return entries.join(';');
}

QPalette deserializePalette(const QString &text)
{
    QPalette palette;
    for (const QString &item : text.split(';', Qt::SkipEmptyParts)) {
        const QString key = item.section('=', 0, 0).trimmed();
        const QColor colour(item.section('=', 1).trimmed());
        if (!colour.isValid()) continue;
        for (const ThemeColourRole &entry : themeColourRoles()) {
            if (key == QLatin1String(entry.key)) setThemeColour(palette, entry, colour);
        }
    }
    return palette;
}

bool isDarkPalette(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < palette.color(QPalette::WindowText).lightness();
}

// The name of the style Qt picked for the platform, before any change
static QString platformStyleName()
{
    static const QString name = QApplication::style()->name();
    return name;
}

// Qt's -style command-line option (or QT_STYLE_OVERRIDE) wins over the setting
static bool isStyleFromCommandLine()
{
    if (!qEnvironmentVariableIsEmpty("QT_STYLE_OVERRIDE")) return true;
    for (const QString &argument : QCoreApplication::arguments()) {
        if (argument == "-style" || argument == "--style" || argument.startsWith("-style=")
            || argument.startsWith("--style=")) {
            return true;
        }
    }
    return false;
}

static bool hasStyle(const QString &name)
{
    return QStyleFactory::keys().contains(name, Qt::CaseInsensitive);
}

// Whether this is Windows 11. Qt's "windows11" style draws with the Segoe
// Fluent Icons font of Windows 11 and does not work on Windows 10.
static bool isWindows11()
{
#ifdef Q_OS_WIN
    return QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11;
#else
    return false;
#endif
}

QStringList availableStyles()
{
    QStringList styles = QStyleFactory::keys();
    if (!isWindows11()) styles.removeIf([](const QString &name) { return name.compare("windows11", Qt::CaseInsensitive) == 0; });
    return styles;
}

QString styleDisplayName(const QString &styleName)
{
    const QString key = styleName.toLower();
    if (key == "windowsvista") return QStringLiteral("Classic (Windows 10)");
    if (key == "windows11") return QStringLiteral("Windows 11");
    if (key == "windows") return QStringLiteral("Windows 9x");
    if (key == "fusion") return QStringLiteral("Fusion");
    if (key == "macos") return QStringLiteral("macOS");
    return styleName;
}

bool isLightOnlyStyle(const QString &styleName)
{
    return styleName.compare("windowsvista", Qt::CaseInsensitive) == 0;
}

// Whether the platform supplies its own dark colours (the Dark theme then uses
// them rather than a palette of ours)
static bool hasSystemDarkScheme()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // QStyleHints::setColorScheme() makes the platform supply dark colours
    return true;
#else
    // Without setColorScheme(), only when the system itself is dark
    return isDarkModeEnabled();
#endif
#else
    // Qt does not follow the GTK theme on Linux, so there is nothing to use
    return false;
#endif
}

// The classic Windows style ("windowsvista") draws buttons, scroll bars, combo
// boxes and the like through the Windows theme engine, whose controls only
// exist in a light version, and it forces a light palette (Qt blog, "Dark Mode
// on Windows 11 with Qt 6.5"). With a dark theme it is replaced by Qt's
// "windows" style, which follows the palette, plus a style sheet giving it the
// flat look of the classic style: the approach taken by OpenMW for the same
// problem, which works on Windows 10 and 11 alike.
static bool classicDarkActive = false;

// Mix two colours: amount 0 gives a, 1 gives b
static QColor mixColours(const QColor &a, const QColor &b, double amount)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * amount,
                            a.greenF() + (b.greenF() - a.greenF()) * amount,
                            a.blueF() + (b.blueF() - a.blueF()) * amount);
}

// The style sheet for the classic style with a dark theme. Backgrounds and
// text refer to the palette (palette(button) and so on), so widgets with a
// palette of their own, such as the highlighted dropouts button, keep it;
// borders and hover/pressed shades are derived from the palette in use, so the
// look follows every theme, custom ones included.
static QString classicDarkStyleSheet(const QPalette &palette)
{
    const QColor button = palette.color(QPalette::Button);
    const QColor text = palette.color(QPalette::ButtonText);
    const QColor window = palette.color(QPalette::Window);
    const QColor highlight = palette.color(QPalette::Highlight);

    const QString border = mixColours(button, text, 0.30).name();
    const QString borderDisabled = mixColours(button, text, 0.15).name();
    const QString textDisabled = mixColours(button, text, 0.45).name();
    const QString pressed = mixColours(button, highlight, 0.35).name();
    const QString handle = mixColours(window, text, 0.30).name();
    const QString handleHover = mixColours(window, text, 0.45).name();
    const QString hover = highlight.name();
    const QString toolTipBase = palette.color(QPalette::ToolTipBase).name();
    const QString toolTipText = palette.color(QPalette::ToolTipText).name();

    return QString(
        "QPushButton, QToolButton {"
        "  background-color: palette(button); color: palette(button-text);"
        "  border: 1px solid %1; padding: 1px 3px; }"
        "QPushButton:hover, QToolButton:hover { border-color: %2; }"
        "QPushButton:pressed, QToolButton:pressed, QPushButton:checked, QToolButton:checked {"
        "  background-color: %3; border-color: %2; }"
        "QPushButton:disabled, QToolButton:disabled { color: %4; border-color: %5; }"
        "QLineEdit, QAbstractSpinBox, QComboBox {"
        "  background-color: palette(base); color: palette(text);"
        "  border: 1px solid %1; padding: 1px 2px;"
        "  selection-background-color: palette(highlight); selection-color: palette(highlighted-text); }"
        "QLineEdit:hover, QAbstractSpinBox:hover, QComboBox:hover { border-color: %2; }"
        "QComboBox QAbstractItemView {"
        "  background-color: palette(base); color: palette(text); border: 1px solid %1; }"
        "QScrollBar:vertical { background: palette(window); width: 17px; }"
        "QScrollBar:horizontal { background: palette(window); height: 17px; }"
        "QScrollBar::handle:vertical { background: %6; min-height: 24px; margin: 2px 4px; }"
        "QScrollBar::handle:horizontal { background: %6; min-width: 24px; margin: 4px 2px; }"
        "QScrollBar::handle:hover { background: %7; }"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; border: none; background: none; }"
        "QScrollBar::add-page, QScrollBar::sub-page { background: none; }"
        "QSlider::groove:horizontal { height: 4px; background: %1; }"
        "QSlider::handle:horizontal { background: palette(highlight); width: 8px; margin: -8px 0px; }"
        "QStatusBar::item { border: none; }"
        "QToolTip { background-color: %8; color: %9; border: 1px solid %1; }")
        .arg(border, hover, pressed, textDisabled, borderDisabled, handle, handleHover, toolTipBase, toolTipText);
}

// Apply the style
static void applyStyle(const QString &styleName, bool isDark)
{
    // Remember the platform default before the first change
    const QString platformName = platformStyleName();
    classicDarkActive = false;
    if (isStyleFromCommandLine()) return;

    QString name = styleName;
    if (name.isEmpty()) {
#ifdef Q_OS_WIN
        // Auto: the classic Windows 10 look, except on Windows 11 with a dark
        // theme, where the Windows 11 style draws the system's dark scheme
        name = (isDark && isWindows11() && hasStyle("windows11")) ? QStringLiteral("windows11")
                                                                  : QStringLiteral("windowsvista");
#else
        name = platformName;
#endif
    }

    if (!availableStyles().contains(name, Qt::CaseInsensitive)) name = platformName;

    // The classic style cannot be dark: the "windows" style and a style sheet
    // stand in for it (see classicDarkStyleSheet)
    if (isDark && isLightOnlyStyle(name)) {
        classicDarkActive = true;
        name = QStringLiteral("windows");
    }

    if (QApplication::style()->name().compare(name, Qt::CaseInsensitive) != 0) {
        if (QStyle *style = QStyleFactory::create(name)) QApplication::setStyle(style);
    }
}

bool applyAppearance(const QString &styleName, ThemeMode mode, const QString &customPalette)
{
    QApplication *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (!app) return false;

    // The palette to set, if any: Light, and Auto on a light system, keep the
    // system's own colours. Qt does not follow the GTK theme on Linux, so Auto
    // detects a dark system itself.
    // Auto is Light or Dark as the system is
    ThemeMode effectiveMode = mode;
    if (mode == ThemeMode::Auto) effectiveMode = isDarkModeEnabled() ? ThemeMode::Dark : ThemeMode::Light;

    bool hasPalette = true;
    bool isDark = false;
    QPalette palette;
    switch (effectiveMode) {
    case ThemeMode::Light:
        // The system's own light colours
        hasPalette = false;
        break;
    case ThemeMode::Dark:
        // The system's own dark colours where it has them, otherwise the
        // Dark grey palette
        isDark = true;
        hasPalette = !hasSystemDarkScheme();
        if (hasPalette) palette = darkPalette();
        break;
    case ThemeMode::Custom:
        hasPalette = !customPalette.isEmpty();
        if (hasPalette) palette = deserializePalette(customPalette);
        isDark = hasPalette && isDarkPalette(palette);
        break;
    default:
        palette = themePalette(mode);
        isDark = isDarkPalette(palette);
        break;
    }

    // PlotWidget::isDarkTheme() reads this, so the graphs match the theme
    app->setProperty("isDarkTheme", isDark);

    // The style first: changing it re-polishes the widgets with its palette
    applyStyle(styleName, isDark);

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // Tell the platform style which scheme to draw, so that a forced theme
    // also holds where the style follows the system (e.g. Windows 11)
    if (mode == ThemeMode::Auto) QGuiApplication::styleHints()->unsetColorScheme();
    else QGuiApplication::styleHints()->setColorScheme(isDark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
#endif

    // Go back to the platform's own colours, so that switching away from dark
    // does not keep the dark ones. A default-constructed palette sets no role,
    // so the application palette falls back to the system/style palette.
    // (QStyle::standardPalette() is not that: on Windows it is the classic
    // grey-beige Windows 2000 palette.)
    app->setPalette(QPalette());

    if (hasPalette) app->setPalette(palette);

    // The classic style's dark look is built from the colours just set, so its
    // style sheet comes last; it is removed again for any other style
    static bool styleSheetSet = false;
    if (classicDarkActive) {
        app->setStyleSheet(classicDarkStyleSheet(app->palette()));
        styleSheetSet = true;
    } else if (styleSheetSet) {
        app->setStyleSheet(QString());
        styleSheetSet = false;
    }

    return isDark;
}
