/******************************************************************************
 * sourceinfopanel.cpp
 * ld-stacker-gui - the information on the source that is chosen
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "sourceinfopanel.h"

#include "framebackend.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QIcon>
#include <QLabel>
#include <QToolButton>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <cmath>

SourceInfoPanel::SourceInfoPanel(QWidget *parent) : QFrame(parent)
{
    setMinimumWidth(280);
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    browser = new QTextBrowser(this);
    browser->setOpenLinks(false);
    layout->addWidget(browser, 1);

    // The mapping of the source (ld-discmap), made from here: a section under the information, a button with its icon and the name beside it
    QFrame *section = new QFrame;
    section->setFrameShape(QFrame::StyledPanel);
    QVBoxLayout *sectionLayout = new QVBoxLayout(section);
    sectionLayout->setContentsMargins(8, 6, 8, 8);
    QLabel *title = new QLabel(tr("Mapping"));
    title->setForegroundRole(QPalette::PlaceholderText);
    sectionLayout->addWidget(title);
    QHBoxLayout *row = new QHBoxLayout;
    mapButton = new QToolButton;
    mapButton->setIcon(QIcon(QStringLiteral(":/icons/map.svg")));
    mapButton->setIconSize(QSize(28, 28));
    mapButton->setAutoRaise(false);
    mapButton->setMinimumSize(44, 44);
    mapButton->setEnabled(false);
    row->addWidget(mapButton);
    QLabel *name = new QLabel(tr("<b>Map</b>"));
    QFont large = name->font();
    large.setPointSizeF(large.pointSizeF() * 1.3);
    name->setFont(large);
    row->addWidget(name);
    mapStatus = new QLabel;
    mapStatus->setWordWrap(true);
    mapStatus->setForegroundRole(QPalette::PlaceholderText);
    row->addWidget(mapStatus, 1);
    sectionLayout->addLayout(row);

    // The SNR of the metadata, measured again from the video and written in them (ld-process-vits)
    QHBoxLayout *snrRow = new QHBoxLayout;
    snrButton = new QToolButton;
    snrButton->setIcon(QIcon(QStringLiteral(":/icons/snr.svg")));
    snrButton->setIconSize(QSize(28, 28));
    snrButton->setAutoRaise(false);
    snrButton->setMinimumSize(44, 44);
    snrButton->setEnabled(false);
    snrRow->addWidget(snrButton);
    QLabel *snrName = new QLabel(tr("<b>Update SNR</b>"));
    snrName->setFont(large);
    snrRow->addWidget(snrName);
    snrStatus = new QLabel;
    snrStatus->setWordWrap(true);
    snrStatus->setForegroundRole(QPalette::PlaceholderText);
    snrRow->addWidget(snrStatus, 1);
    sectionLayout->addLayout(snrRow);
    layout->addWidget(section);
    connect(mapButton, &QToolButton::clicked, this, &SourceInfoPanel::mapRequested);
    connect(snrButton, &QToolButton::clicked, this, &SourceInfoPanel::snrUpdateRequested);
    setMessage(tr("Choose a source to see what is known of it."));
}

void SourceInfoPanel::setMappable(bool mappable, const QString &why)
{
    mapButton->setEnabled(mappable);
    mapButton->setToolTip(mappable ? tr("Map the source with ld-discmap: the frames in the order of the disc, the missing ones padded. You choose to keep a copy beside the capture or to overwrite it.") : why);
    mapStatus->setText(mappable ? tr("Put the frames in the order of the disc") : why);
}

void SourceInfoPanel::setSnrUpdatable(bool updatable, const QString &why)
{
    snrButton->setEnabled(updatable);
    snrButton->setToolTip(updatable ? tr("Measure the SNR of the test signals of every field again and write it in the metadata of the source (ld-process-vits)") : why);
    snrStatus->setText(updatable ? tr("Measure it again and write it in the metadata") : why);
}

void SourceInfoPanel::setMessage(const QString &message)
{
    browser->setHtml(QStringLiteral("<p style='color:gray'>%1</p>").arg(message.toHtmlEscaped()));
}

void SourceInfoPanel::setSource(const SourceInfo &info, const QString &title, bool loaded)
{
    browser->setHtml(describe(info, title, loaded));
}

namespace {

QString row(const QString &name, const QString &value)
{
    return QStringLiteral("<tr><td style='color:gray' valign='top'>%1&nbsp;&nbsp;</td><td>%2</td></tr>").arg(name.toHtmlEscaped(), value.toHtmlEscaped());
}

QString heading(const QString &text)
{
    return QStringLiteral("<tr><td colspan='2'><br><b>%1</b></td></tr>").arg(text.toHtmlEscaped());
}

QString systemName(VideoSystem system)
{
    return system == NTSC ? QStringLiteral("NTSC") : (system == PAL ? QStringLiteral("PAL") : QStringLiteral("PAL-M"));
}

QString clock(qint32 seconds)
{
    return QStringLiteral("%1:%2:%3").arg(seconds / 3600).arg((seconds / 60) % 60, 2, 10, QChar('0')).arg(seconds % 60, 2, 10, QChar('0'));
}

QString mean(const QVector<float> &values)
{
    double total = 0;
    qint32 count = 0;
    for (const float value: values) {
        if (!std::isfinite(value)) continue;
        total += value;
        count++;
    }
    return count ? QStringLiteral("%1 dB (%2 frames)").arg(total / count, 0, 'f', 1).arg(count) : QStringLiteral("-");
}

} // namespace

QString SourceInfoPanel::describe(const SourceInfo &info, const QString &title, bool loaded)
{
    QString html = QStringLiteral("<h3>%1</h3><table cellspacing='0' cellpadding='1'>").arg(title.toHtmlEscaped());
    const QFileInfo file(info.file);
    html += row(tr("File"), info.file);
    if (!loaded) {
        html += row(tr("State"), info.error.isEmpty() ? tr("not loaded") : info.error);
        return html + QStringLiteral("</table>");
    }
    html += row(tr("Kind"), info.cvbs ? tr("CVBS capture") : tr("TBC capture"));
    html += row(tr("Metadata"), info.metadataFile.isEmpty() ? QStringLiteral("-") : QFileInfo(info.metadataFile).fileName() + QStringLiteral(" (") + info.metadataFormat + QStringLiteral(")"));

    html += heading(tr("Video"));
    html += row(tr("System"), systemName(info.system) + (info.widescreen ? tr(", widescreen") : QString()));
    html += row(tr("Frames"), tr("%1 (%2 fields)").arg(info.frames).arg(info.fields));
    html += row(tr("Field size"), QStringLiteral("%1 x %2").arg(info.fieldWidth).arg(info.fieldHeight));
    if (info.sampleRate > 0) html += row(tr("Sample rate"), QStringLiteral("%1 MHz").arg(info.sampleRate / 1.0e6, 0, 'f', 3));
    if (info.fsc > 0) html += row(tr("Subcarrier"), QStringLiteral("%1 MHz").arg(info.fsc / 1.0e6, 0, 'f', 6));
    if (info.blackIre >= 0 && info.whiteIre >= 0) html += row(tr("Black, white"), QStringLiteral("%1, %2 (16-bit)").arg(info.blackIre).arg(info.whiteIre));
    if (!info.tapeFormat.isEmpty()) html += row(tr("Tape format"), info.tapeFormat);
    if (!info.gitCommit.isEmpty()) html += row(tr("Decoded by"), info.gitCommit);

    html += heading(tr("Disc (VBI)"));
    if (info.discType == 0 && info.vbiFrames == 0) {
        html += row(tr("VBI"), tr("no frame numbers or time codes"));
    } else {
        html += row(tr("Disc"), info.discType == 2 ? tr("CAV") : (info.discType == 1 ? tr("CLV") : tr("unknown")));
        if (info.firstPicture >= 0) html += row(tr("Pictures"), QStringLiteral("%1 to %2").arg(info.firstPicture).arg(info.lastPicture));
        if (info.firstSeconds >= 0) html += row(tr("Times"), QStringLiteral("%1 to %2").arg(clock(info.firstSeconds), clock(info.lastSeconds)));
        html += row(tr("Chapters"), QString::number(info.chapters));
        html += row(tr("Frames with VBI"), QStringLiteral("%1 of %2").arg(info.vbiFrames).arg(info.frames));
        html += row(tr("Numbering jumps"), QString::number(info.jumps));
        html += row(tr("Repeated numbers"), QString::number(info.duplicates));
    }
    html += row(tr("Padded frames"), QString::number(info.paddedFrames));

    html += heading(tr("Mapping"));
    // (a source read through a map has the view's metadata, which say it is mapped: what matters is where the map is kept)
    QString mapped;
    if (info.throughMap) mapped = info.mapInSource ? tr("yes (read through the map kept in the source)") : tr("yes (read through the map of the session)");
    else if (info.mapInSource) mapped = tr("no (the source keeps a map: Follow map is off)");
    else mapped = info.isMapped ? tr("yes (the metadata says so)") : tr("no");
    html += row(tr("Mapped"), mapped);

    html += heading(tr("SNR of the metadata"));
    html += row(tr("White SNR"), mean(info.whiteSnr));
    html += row(tr("Black PSNR"), mean(info.blackSnr));

    html += QStringLiteral("</table>");

    // The history of an LDX file: who did what, when, and with what
    if (!info.history.isEmpty()) {
        html += QStringLiteral("<p><b>%1</b></p><table cellspacing='0' cellpadding='2'>").arg(tr("History").toHtmlEscaped());
        for (const QJsonValue &value: info.history) {
            const QJsonObject entry = value.toObject();
            QString arguments;
            for (const QJsonValue &argument: entry.value("arguments").toArray()) arguments += (arguments.isEmpty() ? QString() : QStringLiteral(" ")) + argument.toString();
            QString line = entry.value("tool").toString();
            if (entry.contains("version")) line += QStringLiteral(" ") + entry.value("version").toString();
            line += QStringLiteral(": ") + entry.value("command").toString();
            if (entry.contains("detail")) line += QStringLiteral(" - ") + entry.value("detail").toString();
            html += QStringLiteral("<tr><td style='color:gray' valign='top'>%1&nbsp;&nbsp;</td><td>%2%3</td></tr>")
                        .arg(entry.value("time").toString().toHtmlEscaped(), line.toHtmlEscaped(),
                             arguments.isEmpty() ? QString() : QStringLiteral("<br><span style='color:gray'>%1</span>").arg(arguments.toHtmlEscaped()));
        }
        html += QStringLiteral("</table>");
    }
    return html;
}
