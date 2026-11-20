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
        if (number.hasMatch()) {
            option.name = number.captured(1);
            option.minimum = number.captured(2).toDouble();
            option.maximum = number.captured(3).toDouble();
            option.isInteger = !number.captured(2).contains('.') && !number.captured(3).contains('.');
        } else if (flag.hasMatch()) {
            option.name = flag.captured(1);
            option.isBoolean = true;
            option.maximum = 1.0;
        } else {
            continue;
        }
        // (the description is the next line, if it is text and not another option)
        if (i + 1 < lines.size()) {
            const QString next = lines[i + 1].trimmed();
            if (!next.isEmpty() && !next.startsWith("-")) option.description = next;
        }
        QVector<Option> &options = help.modules[module];
        bool repeated = false;
        for (const Option &known: options) repeated = repeated || known.name == option.name;
        if (!repeated) options.push_back(option);
    }
    return true;
}

bool load(const QString &vlc, Help &help, QString &error, bool refresh)
{
    const QFileInfo info(vlc);
    if (!info.isFile()) {
        error = QObject::tr("VLC was not found at %1").arg(vlc);
        return false;
    }
    // What VLC said the first time it was asked about this file: it is asked again only when the person says so (refresh), not when VLC is updated
    const QString key = QString::fromLatin1(QCryptographicHash::hash(info.absoluteFilePath().toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString cacheName = directory + "/vlc-help-" + key + ".txt";
    QFile cache(cacheName);
    if (!refresh && cache.open(QIODevice::ReadOnly)) {
        const QString text = QString::fromUtf8(cache.readAll());
        if (parse(text, help)) return true;
    }

    // On Windows a VLC that has no console (it is run by a program that has none) does not print its help: it writes it to vlc-help.txt in its working directory,
    // and waits for a key to be pressed on a console that is not there. So it is run in a directory of its own, and the file is read when it is there and is
    // complete (its size stays the same), and VLC is let go. Elsewhere, or if there is no file, the help is what it printed.
    QTemporaryDir workDirectory;
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setStandardInputFile(QProcess::nullDevice());
    if (workDirectory.isValid()) process.setWorkingDirectory(workDirectory.path());
    process.start(vlc, { "-I", "dummy", "--quiet", "-H", "--advanced" });
    if (!process.waitForStarted(10000)) {
        error = QObject::tr("VLC could not be run (%1)").arg(vlc);
        return false;
    }
    QString text;
    const QString written = workDirectory.path() + "/vlc-help.txt";
    qint64 lastSize = -1;
    for (int waited = 0; waited < 40000; waited += 250) {
        if (process.waitForFinished(250)) {
            text = QString::fromUtf8(process.readAllStandardOutput());
            break;
        }
        const qint64 size = QFileInfo(written).size();
        if (size > 1000 && size == lastSize) {
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
    if (text.isEmpty()) {
        error = QObject::tr("VLC did not answer (%1)").arg(vlc);
        return false;
    }
    if (!parse(text, help)) {
        error = QObject::tr("VLC did not list its video outputs: %1 may not be a VLC").arg(vlc);
        return false;
    }
    QDir().mkpath(directory);
    QFile out(cacheName);
    if (out.open(QIODevice::WriteOnly)) out.write(text.toUtf8());
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
