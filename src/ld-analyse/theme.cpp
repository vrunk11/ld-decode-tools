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
#include <QProxyStyle>
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

QStringList availableStyles()
{
    return QStyleFactory::keys();
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
// them rather than a palette of ours). The classic style gets the Windows 11
// ones through ClassicStyle.
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

// The classic Windows style ("windowsvista") draws buttons, scroll bars,
// combo boxes, menus and the like through the Windows theme engine, which only
// knows a light appearance and ignores the palette. With a dark theme this
// style keeps the classic layout and sizes but paints everything with the
// Windows 11 style instead (Fusion where that is not available), which draws
// a proper dark appearance. Complex controls also take their sub-control
// geometry from the painting style, so that what is clicked matches what is
// drawn. With a light theme it is the classic style, unchanged.
class ClassicStyle : public QProxyStyle
{
public:
    // A style instance is either light or dark for its whole life: applyStyle()
    // sets a new one when the theme switches, so polish and unpolish always
    // go to the same style
    explicit ClassicStyle(bool _dark)
        : QProxyStyle(QStyleFactory::create("windowsvista")), dark(_dark)
    {
        darkStyle = QStyleFactory::create("windows11");
        if (!darkStyle) darkStyle = QStyleFactory::create("fusion");
    }

    ~ClassicStyle() override
    {
        delete darkStyle;
    }

    // The base colours (used where the theme sets no palette, i.e. Auto) come
    // from the painting style too: the classic style forces a light palette
    // even on a dark system, which left light backgrounds behind the dark
    // controls
    QPalette standardPalette() const override
    {
        return isDark() ? darkStyle->standardPalette() : QProxyStyle::standardPalette();
    }

    void polish(QPalette &palette) override
    {
        if (isDark()) darkStyle->polish(palette);
        else QProxyStyle::polish(palette);
    }

    void polish(QWidget *widget) override
    {
        if (isDark()) darkStyle->polish(widget);
        else QProxyStyle::polish(widget);
    }

    void unpolish(QWidget *widget) override
    {
        if (isDark()) darkStyle->unpolish(widget);
        else QProxyStyle::unpolish(widget);
    }

    void polish(QApplication *application) override
    {
        if (isDark()) darkStyle->polish(application);
        else QProxyStyle::polish(application);
    }

    void unpolish(QApplication *application) override
    {
        if (isDark()) darkStyle->unpolish(application);
        else QProxyStyle::unpolish(application);
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget = nullptr) const override
    {
        if (isDark()) darkStyle->drawPrimitive(element, option, painter, widget);
        else QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter,
                     const QWidget *widget = nullptr) const override
    {
        if (isDark()) darkStyle->drawControl(element, option, painter, widget);
        else QProxyStyle::drawControl(element, option, painter, widget);
    }

    void drawComplexControl(ComplexControl control, const QStyleOptionComplex *option, QPainter *painter,
                            const QWidget *widget = nullptr) const override
    {
        if (isDark()) darkStyle->drawComplexControl(control, option, painter, widget);
        else QProxyStyle::drawComplexControl(control, option, painter, widget);
    }

    QRect subControlRect(ComplexControl control, const QStyleOptionComplex *option, SubControl subControl,
                         const QWidget *widget = nullptr) const override
    {
        if (isDark()) return darkStyle->subControlRect(control, option, subControl, widget);
        return QProxyStyle::subControlRect(control, option, subControl, widget);
    }

    SubControl hitTestComplexControl(ComplexControl control, const QStyleOptionComplex *option, const QPoint &position,
                                     const QWidget *widget = nullptr) const override
    {
        if (isDark()) return darkStyle->hitTestComplexControl(control, option, position, widget);
        return QProxyStyle::hitTestComplexControl(control, option, position, widget);
    }

    QRect subElementRect(SubElement element, const QStyleOption *option, const QWidget *widget = nullptr) const override
    {
        if (isDark()) return darkStyle->subElementRect(element, option, widget);
        return QProxyStyle::subElementRect(element, option, widget);
    }

    // Spin boxes and combo boxes lay their buttons out differently (Windows 11
    // puts the spin box arrows side by side), so they take the painting
    // style's size, or the arrows would cover the text. Everything else keeps
    // the classic, more compact sizes.
    QSize sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &contentsSize,
                           const QWidget *widget = nullptr) const override
    {
        if (isDark() && (type == CT_SpinBox || type == CT_ComboBox)) {
            return darkStyle->sizeFromContents(type, option, contentsSize, widget);
        }
        return QProxyStyle::sizeFromContents(type, option, contentsSize, widget);
    }

private:
    QStyle *darkStyle = nullptr;

    const bool dark;

    bool isDark() const
    {
        return dark;
    }
};

static bool isClassicStyleInUse()
{
    return dynamic_cast<ClassicStyle *>(QApplication::style()) != nullptr;
}

// Apply the style and return the name of the one in use
static QString applyStyle(const QString &styleName, bool isDark)
{
    // Remember the platform default before the first change
    const QString platformName = platformStyleName();
    if (isStyleFromCommandLine()) return QApplication::style()->name();

    QString name = styleName;
    if (name.isEmpty()) {
#ifdef Q_OS_WIN
        // Auto: the classic Windows 10 look when light; when dark, the
        // Windows 11 style, whose own dark scheme matches the system
        name = isDark ? QStringLiteral("windows11") : QStringLiteral("windowsvista");
        if (!hasStyle(name)) name = isDark ? QStringLiteral("fusion") : platformName;
#else
        name = platformName;
#endif
    }

    if (!hasStyle(name)) name = platformName;

    // The classic style is wrapped so that it can also be dark (ClassicStyle).
    // Switching between light and dark changes some of its sizes, so it is set
    // again then, which makes every widget recompute its size and layout.
    static bool classicIsDark = false;
    if (isLightOnlyStyle(name)) {
        if (!isClassicStyleInUse() || classicIsDark != isDark) QApplication::setStyle(new ClassicStyle(isDark));
        classicIsDark = isDark;
        return name;
    }

    if (isClassicStyleInUse() || QApplication::style()->name().compare(name, Qt::CaseInsensitive) != 0) {
        if (QStyle *style = QStyleFactory::create(name)) QApplication::setStyle(style);
    }
    return QApplication::style()->name();
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

    // Set before the style, whose painting and sizes (ClassicStyle) depend on
    // it; PlotWidget::isDarkTheme() also reads it, so the graphs match
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

    return isDark;
}
