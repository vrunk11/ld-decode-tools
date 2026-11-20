/******************************************************************************
 * vlcsupport.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "vlcsupport.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace VlcSupport {

QString find(const QString &configured)
{
    if (!configured.isEmpty() && QFileInfo(configured).isFile()) return configured;
    QStringList candidates;
#ifdef Q_OS_WIN
    for (const char *variable: { "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432" }) {
        const QString base = qEnvironmentVariable(variable);
        if (!base.isEmpty()) candidates << base + "/VideoLAN/VLC/vlc.exe";
    }
#elif defined(Q_OS_MACOS)
    candidates << "/Applications/VLC.app/Contents/MacOS/VLC";
#endif
    for (const QString &candidate: candidates) {
        if (QFileInfo(candidate).isFile()) return candidate;
    }
    return QStandardPaths::findExecutable("vlc");
}

bool parse(const QString &text, Help &help)
{
    // --vout={any,direct3d11,...} and --deinterlace-mode={auto,blend,...}
    const auto choices = [&text](const char *option) {
        const QRegularExpression expression(QString("--%1=\\{([^}]*)\\}").arg(option));
        const QRegularExpressionMatch match = expression.match(text);
        QStringList list = match.hasMatch() ? match.captured(1).split(',', Qt::SkipEmptyParts) : QStringList();
        for (QString &item: list) item = item.trimmed();
        list.removeDuplicates();
        return list;
    };
    help.videoOutputs = choices("vout");
    help.deinterlaceModes = choices("deinterlace-mode");
    // (any is VLC's own choice, none shows nothing, auto is the setting of VLC)
    help.videoOutputs.removeAll("any");
    help.videoOutputs.removeAll("none");
    help.deinterlaceModes.removeAll("auto");
    if (help.videoOutputs.isEmpty() && help.deinterlaceModes.isEmpty()) return false;

    // The sections of the help: " Temporal scaling (OpenCL GPU) (tscale)", a line with one space and the name of the module at its end, then its options:
    //   "      --sharpen-sigma=<Float [0.000000 .. 2.000000]> " and, on the next line, what it is; "--grain-period-min=<Integer [1 .. 64]>"
    static const QRegularExpression header("^ (\\S.*) \\(([A-Za-z0-9_]+)\\)\\s*$");
    static const QRegularExpression ranged("^\\s+(?:-\\w, )?--([A-Za-z0-9-]+)=<[^\\[<>]*\\[\\s*(-?[0-9.]+)\\s*\\.\\.\\s*(-?[0-9.]+)\\s*\\]>");
    static const QRegularExpression boolean("^\\s+(?:-\\w, )?--([A-Za-z0-9-]+), --no-");
    // "--tscale-height=<Integer>" (a number with no range: the word is in the language of VLC), "--wkdif-order={0 (Auto), 1 (Top first)}" or "--wkdif-mode={auto,blend}"
    static const QRegularExpression unranged("^\\s+(?:-\\w, )?--([A-Za-z0-9-]+)=<([^\\[<>]*)>(?:\\s+(\\S.*))?$");
    static const QRegularExpression listed("^\\s+(?:-\\w, )?--([A-Za-z0-9-]+)=\\{(.*)\\}\\s*$");
    const QStringList lines = text.split('\n');
    QString module = QStringLiteral("core");
    for (qsizetype i = 0; i < lines.size(); i++) {
        const QRegularExpressionMatch section = header.match(lines[i]);
        if (section.hasMatch()) {
            module = section.captured(2);
            help.moduleNames.insert(module, section.captured(1));
            continue;
        }
        Option option;
        const QRegularExpressionMatch number = ranged.match(lines[i]);
        const QRegularExpressionMatch flag = boolean.match(lines[i]);
        const QRegularExpressionMatch plain = unranged.match(lines[i]);
        const QRegularExpressionMatch list = listed.match(lines[i]);
        if (number.hasMatch()) {
            option.name = number.captured(1);
            option.minimum = number.captured(2).toDouble();
            option.maximum = number.captured(3).toDouble();
            option.isInteger = !number.captured(2).contains('.') && !number.captured(3).contains('.');
        } else if (flag.hasMatch()) {
            option.name = flag.captured(1);
            option.isBoolean = true;
            option.maximum = 1.0;
        } else if (plain.hasMatch()) {
            // (a number of any value; text is not offered)
            const QString type = plain.captured(2).toLower();
            if (type.contains("float") || type.contains("flottant")) {
                option.isInteger = false;
            } else if (type.contains("integer") || type.contains("entier")) {
                option.isInteger = true;
            } else {
                continue;
            }
            option.name = plain.captured(1);
            option.minimum = -1000000.0;
            option.maximum = 1000000.0;
            // (its name may be on the same line: "--tscale-height=<Integer>   Output height (-1 = automatic)")
            if (!plain.captured(3).trimmed().isEmpty()) option.description = plain.captured(3).trimmed();
        } else if (list.hasMatch()) {
            option.name = list.captured(1);
            option.isChoice = true;
            // "0 (Auto (on the picture))" is the value 0, called "Auto (on the picture)"; "blend" is its own name
            static const QRegularExpression labelled("^(\\S+) \\((.*)\\)$");
            for (const QString &item: list.captured(2).split(',', Qt::SkipEmptyParts)) {
                const QString entry = item.trimmed();
                const QRegularExpressionMatch named = labelled.match(entry);
                option.choices << (named.hasMatch() ? named.captured(1) : entry);
                option.choiceLabels << (named.hasMatch() ? named.captured(2) : entry);
            }
            if (option.choices.isEmpty()) continue;
        } else {
            continue;
        }
        // (the description is the next line, if it is text and not another option; a switch then says whether it is on by default, in the language of VLC)
        if (i + 1 < lines.size()) {
            const QString next = lines[i + 1].trimmed();
            if (option.description.isEmpty() && !next.isEmpty() && !next.startsWith("-")) option.description = next;
        }
        if (option.isBoolean) {
            for (qsizetype j = i + 1; j < qMin(lines.size(), i + 3); j++) {
                const QString note = lines[j].trimmed().toLower();
                if (!note.startsWith('(')) continue;
                if (note.contains("disabled") || note.contains("sactiv")) {
                    option.hasDefault = true;
                    option.defaultValue = 0.0;
                } else if (note.contains("enabled") || note.contains("activ")) {
                    option.hasDefault = true;
                    option.defaultValue = 1.0;
                }
            }
        }
        QVector<Option> &options = help.modules[module];
        bool repeated = false;
        for (const Option &known: options) repeated = repeated || known.name == option.name;
        if (!repeated) options.push_back(option);
    }
    return true;
}

void parseModules(const QString &text, Help &help)
{
    // "  wkdif                  wkdif : deterministic deinterlacer (GPU OpenCL)", then "   s deinterlace" (what it answers to) and "   c video filter (10)" (what it is,
    // and its score: the module of a kind that answers to a name with the highest score that accepts is the one VLC takes)
    struct Module {
        QString name, description;
        QStringList shortcuts;
        QString capability;
        int score = 0;
    };
    QVector<Module> modules;
    static const QRegularExpression header("^  (\\S+)\\s+(\\S.*)$");
    static const QRegularExpression shortcuts("^   s (.*)$");
    static const QRegularExpression capability("^   c (.*) \\((-?\\d+)\\)\\s*$");
    for (const QString &raw: text.split('\n')) {
        QString line = raw;
        line.remove('\r');
        if (line.trimmed().isEmpty()) continue;
        if (const QRegularExpressionMatch s = shortcuts.match(line); s.hasMatch() && !modules.isEmpty()) {
            modules.last().shortcuts << s.captured(1).split(' ', Qt::SkipEmptyParts);
        } else if (const QRegularExpressionMatch c = capability.match(line); c.hasMatch() && !modules.isEmpty()) {
            modules.last().capability = c.captured(1).trimmed();
            modules.last().score = c.captured(2).toInt();
        } else if (const QRegularExpressionMatch h = header.match(line); h.hasMatch()) {
            Module module;
            module.name = h.captured(1);
            module.description = h.captured(2).trimmed();
            modules.push_back(module);
        }
    }
    // VLC's own deinterlacer is the module named deinterlace; the ones that answer to that name with a higher score are tried before it: they are the ones that were
    // added (a module that declines what it is given leaves the picture to the next)
    int ownScore = 0;
    for (const Module &module: modules) {
        if (module.name == "deinterlace" && module.capability == "video filter") ownScore = module.score;
    }
    help.deinterlacers.clear();
    for (const Module &module: modules) {
        if (module.name == "deinterlace" || module.capability != "video filter" || !module.shortcuts.contains("deinterlace") || module.score <= ownScore) continue;
        if (help.deinterlacers.contains(module.name)) continue;
        help.deinterlacers << module.name;
        if (!help.moduleNames.contains(module.name)) help.moduleNames.insert(module.name, module.description);
    }
}

// Where VLC keeps its preferences
static QString preferencesPath()
{
#if defined(Q_OS_WIN)
    return qEnvironmentVariable("APPDATA") + "/vlc/vlcrc";
#elif defined(Q_OS_MACOS)
    return QDir::homePath() + "/Library/Preferences/org.videolan.vlc/vlcrc";
#else
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/vlc/vlcrc";
#endif
}

void applyConfiguration(const QString &text, Help &help)
{
    // "#tscale-height=-1" (the module's value: a comment) and "tscale-mode=6" (set in VLC: it wins); "# Scaling algorithm (Integer)" is a comment that names it
    static const QRegularExpression line("^(#?)([A-Za-z0-9][A-Za-z0-9-]*)=(.*)$");
    QMap<QString, QString> values;
    help.vlcSettings.clear();
    for (const QString &raw: text.split('\n')) {
        const QRegularExpressionMatch match = line.match(raw.trimmed());
        if (!match.hasMatch()) continue;
        const QString name = match.captured(2);
        const QString value = match.captured(3).trimmed();
        if (match.captured(1).isEmpty()) {
            help.vlcSettings.insert(name, value);
            values.insert(name, value);
        } else if (!help.vlcSettings.contains(name)) {
            values.insert(name, value);
        }
    }
    for (auto module = help.modules.begin(); module != help.modules.end(); ++module) {
        for (Option &option: module.value()) {
            const auto found = values.constFind(option.name);
            if (found == values.constEnd()) continue;
            if (option.isChoice) {
                if (option.choices.contains(found.value())) option.defaultText = found.value();
                continue;
            }
            bool ok = false;
            const double value = found.value().toDouble(&ok);
            if (!ok) continue;
            option.defaultValue = option.isBoolean ? (value != 0.0 ? 1.0 : 0.0) : qBound(option.minimum, value, option.maximum);
            option.hasDefault = true;
        }
    }
}

// Run VLC with the arguments and take what it prints. On Windows a VLC that has no console (it is run by a program that has none) does not print it: it writes it to
// vlc-help.txt in its working directory, and waits for a key to be pressed on a console that is not there. So it is run in a directory of its own, and the file is
// read when it is there and is complete (its size stays the same), and VLC is let go. Elsewhere, or if there is no file, the text is what it printed.
static QString askVlc(const QString &vlc, const QStringList &arguments, QString &error)
{
    QTemporaryDir workDirectory;
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setStandardInputFile(QProcess::nullDevice());
    if (workDirectory.isValid()) process.setWorkingDirectory(workDirectory.path());
    process.start(vlc, QStringList{ "-I", "dummy", "--quiet" } + arguments);
    if (!process.waitForStarted(10000)) {
        error = QObject::tr("VLC could not be run (%1)").arg(vlc);
        return QString();
    }
    QString text;
    const QString written = workDirectory.path() + "/vlc-help.txt";
    // (VLC writes it in parts, and stops between them while it loads its modules: the file is complete when its size has not changed for two seconds)
    qint64 lastSize = -1;
    int steady = 0;
    for (int waited = 0; waited < 60000; waited += 250) {
        if (process.waitForFinished(250)) {
            text = QString::fromUtf8(process.readAllStandardOutput());
            break;
        }
        const qint64 size = QFileInfo(written).size();
        steady = (size > 1000 && size == lastSize) ? steady + 1 : 0;
        if (steady >= 8) {
            QFile file(written);
            if (file.open(QIODevice::ReadOnly)) text = QString::fromUtf8(file.readAll());
            break;
        }
        lastSize = size;
    }
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(3000);
    }
    if (text.isEmpty() && QFileInfo(written).exists()) {
        QFile file(written);
        if (file.open(QIODevice::ReadOnly)) text = QString::fromUtf8(file.readAll());
    }
    if (text.isEmpty()) error = QObject::tr("VLC did not answer (%1)").arg(vlc);
    return text;
}

// What VLC said the first time it was asked this about this file (name is help or modules), or it is asked now and what it says is kept. It is asked again only when
// the person says so (refresh), not when VLC is updated.
// The file that keeps what was had of this VLC (name is help, modules or config)
static QString cachedPath(const QString &vlc, const QString &name)
{
    const QString key = QString::fromLatin1(QCryptographicHash::hash(QFileInfo(vlc).absoluteFilePath().toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(directory);
    return directory + "/vlc-" + name + "-" + key + ".txt";
}

static QString cachedAnswer(const QString &vlc, const QString &name, const QStringList &arguments, bool refresh, QString &error)
{
    const QString cacheName = cachedPath(vlc, name);
    QFile cache(cacheName);
    if (!refresh && cache.open(QIODevice::ReadOnly)) {
        const QString text = QString::fromUtf8(cache.readAll());
        if (!text.isEmpty()) return text;
    }
    const QString text = askVlc(vlc, arguments, error);
    if (text.isEmpty()) return text;
    QFile out(cacheName);
    if (out.open(QIODevice::WriteOnly)) out.write(text.toUtf8());
    return text;
}

bool load(const QString &vlc, Help &help, QString &error, bool refresh)
{
    if (!QFileInfo(vlc).isFile()) {
        error = QObject::tr("VLC was not found at %1").arg(vlc);
        return false;
    }
    // The options of the modules (--full-help), then the modules themselves with what they do (--list-verbose: the deinterlacers that were added are found there)
    QString text = cachedAnswer(vlc, "help", { "-H", "--advanced" }, refresh, error);
    if (text.isEmpty()) return false;
    if (!parse(text, help) && !refresh) {
        // (what was kept cannot be read: VLC is asked again)
        help = Help();
        text = cachedAnswer(vlc, "help", { "-H", "--advanced" }, true, error);
        if (text.isEmpty()) return false;
        parse(text, help);
    }
    if (help.videoOutputs.isEmpty() && help.deinterlaceModes.isEmpty()) {
        // (an answer that cannot be read is not kept)
        QFile::remove(cachedPath(vlc, "help"));
        error = QObject::tr("VLC did not list its video outputs: %1 may not be a VLC").arg(vlc);
        return false;
    }
    QString listError;
    const QString modules = cachedAnswer(vlc, "modules", { "--list-verbose" }, refresh, listError);
    if (!modules.isEmpty()) parseModules(modules, help);
    // The preferences of VLC, as they were when VLC was asked (they are read again with it, when the person asks: not each time, as VLC's answers)
    QFile preferences(preferencesPath());
    QString preferencesText;
    if (refresh || !QFileInfo::exists(cachedPath(vlc, "config"))) {
        if (preferences.open(QIODevice::ReadOnly)) preferencesText = QString::fromUtf8(preferences.readAll());
        QFile out(cachedPath(vlc, "config"));
        if (out.open(QIODevice::WriteOnly)) out.write(preferencesText.toUtf8());
    } else {
        QFile cached(cachedPath(vlc, "config"));
        if (cached.open(QIODevice::ReadOnly)) preferencesText = QString::fromUtf8(cached.readAll());
    }
    applyConfiguration(preferencesText, help);
    return true;
}

// What VLC offers as far as is known, for when it cannot be asked (on Windows a VLC that is run by a program that has no console waits for a key to be pressed on
// a console that is not there, rather than print its help): the usual video outputs and deinterlacers, and the options of the modules of the usual adjustments, as
// VLC 3 has them
void fallback(Help &help)
{
    help = Help();
    help.videoOutputs = { "direct3d11", "direct3d9", "glwin32", "gl", "directdraw", "wingdi" };
    help.deinterlaceModes = { "discard", "blend", "mean", "bob", "linear", "x", "yadif", "yadif2x", "phosphor", "ivtc" };
    const auto number = [](const char *name, const char *description, double minimum, double maximum, bool isInteger) {
        Option option;
        option.name = name;
        option.description = description;
        option.minimum = minimum;
        option.maximum = maximum;
        option.isInteger = isInteger;
        return option;
    };
    help.modules["adjust"] = { number("contrast", "Contrast (0-2)", 0.0, 2.0, false), number("brightness", "Brightness (0-2)", 0.0, 2.0, false),
                               number("hue", "Hue (-180..180)", -180.0, 180.0, false), number("saturation", "Saturation (0-3)", 0.0, 3.0, false),
                               number("gamma", "Gamma (0-10)", 0.01, 10.0, false) };
    help.modules["sharpen"] = { number("sharpen-sigma", "Sharpen strength (0-2)", 0.0, 2.0, false) };
    help.modules["gradfun"] = { number("gradfun-radius", "Radius", 4.0, 32.0, true), number("gradfun-strength", "Strength", 0.51, 255.0, false) };
    help.modules["grain"] = { number("grain-variance", "Variance", 0.0, 10.0, false), number("grain-period-min", "Minimal period", 1.0, 64.0, true),
                              number("grain-period-max", "Maximal period", 1.0, 64.0, true) };
    help.moduleNames["adjust"] = "Image adjustments";
    help.moduleNames["sharpen"] = "Sharpen";
    help.moduleNames["gradfun"] = "Banding removal";
    help.moduleNames["grain"] = "Film grain";
}

double defaultOf(const Option &option)
{
    if (option.hasDefault) return option.defaultValue;
    // (a number of any value starts at 0)
    if (option.minimum <= -1000000.0) return 0.0;
    static const struct { const char *name; double value; } DEFAULTS[] = {
        { "contrast", 1.0 }, { "brightness", 1.0 }, { "hue", 0.0 }, { "saturation", 1.0 }, { "gamma", 1.0 },
        { "sharpen-sigma", 0.05 }, { "gradfun-radius", 16.0 }, { "gradfun-strength", 1.2 }, { "grain-variance", 0.5 },
        { "grain-period-min", 1.0 }, { "grain-period-max", 48.0 },
    };
    for (const auto &entry: DEFAULTS) {
        if (option.name == QLatin1String(entry.name)) return qBound(option.minimum, entry.value, option.maximum);
    }
    return option.minimum;
}

}
