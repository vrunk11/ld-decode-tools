/******************************************************************************
 * sourceinfodialog.cpp
 * ld-analyse - a small window with the cover of the file and what is known of the source that is open
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "sourceinfodialog.h"

#include "captureinfo.h"
#include "coverwidget.h"
#include "ldxcapture.h"
#include "tbcsource.h"

#include <QDateTime>
#include <QDir>
#include <QFrame>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {

constexpr int kMaxCoverHeight = 400;
constexpr int kCoverMargins = 16 + 4;     // the margins of the window, and the border of the frame

QString row(const QString &name, const QString &value)
{
    return QStringLiteral("<tr><td style='color:gray' valign='top'>%1&nbsp;&nbsp;</td><td>%2</td></tr>").arg(name.toHtmlEscaped(), value.toHtmlEscaped());
}

QString heading(const QString &text)
{
    return QStringLiteral("<tr><td colspan='2'><br><b>%1</b></td></tr>").arg(text.toHtmlEscaped());
}

}

QString SourceInfoDialog::describe(TbcSource &source)
{
    const QString file = source.getCurrentSourceFilename();
    if (file.isEmpty()) return tr("<p><i>No source is open.</i></p>");
    const LdDecodeMetaData::VideoParameters &video = source.getVideoParameters();

    QString path = file;
    qint32 track = 0;
    const bool ldx = LdxCapture::isLdxName(file);
    if (ldx) LdxCapture::split(file, path, track);

    QString html = QStringLiteral("<table cellspacing='2'>");
    html += row(tr("File"), QDir::toNativeSeparators(path));
    if (ldx && track > 0) html += row(tr("Track"), QString::number(track));
    html += row(tr("Size"), tr("%1 MB").arg(QFileInfo(path).size() / 1048576.0, 0, 'f', 1));
    html += row(tr("Metadata"), source.getMetadataFormatName());

    html += heading(tr("Video"));
    html += row(tr("System"), source.getSystemDescription());
    html += row(tr("Frames"), tr("%1 (%2 fields)").arg(source.getNumberOfFrames()).arg(source.getNumberOfFields()));
    if (video.fieldWidth > 0) html += row(tr("Field size"), QStringLiteral("%1 × %2").arg(video.fieldWidth).arg(video.fieldHeight));
    if (video.sampleRate > 0) html += row(tr("Sample rate"), QStringLiteral("%1 MHz").arg(video.sampleRate / 1.0e6, 0, 'f', 4));
    if (video.black16bIre > 0 || video.white16bIre > 0) html += row(tr("Black, white"), QStringLiteral("%1, %2 (16-bit)").arg(video.black16bIre).arg(video.white16bIre));
    html += row(tr("Shape"), source.getIsWidescreen() ? tr("16:9 (widescreen)") : tr("4:3"));
    if (!video.tapeFormat.isEmpty()) html += row(tr("Tape format"), video.tapeFormat);
    if (!video.decoder.isEmpty()) html += row(tr("Decoded by"), video.decoder);

    // What a person said of the capture, and where it comes from (an LDX file)
    {
        const QVector<CaptureInfo::Row> said = CaptureInfo::saidRows(file);
        if (!said.isEmpty()) {
            html += heading(tr("Capture"));
            for (const CaptureInfo::Row &item : said) html += row(item.name, item.value);
        }
        const QVector<CaptureInfo::Row> from = CaptureInfo::rfSourceRows(file);
        if (!from.isEmpty()) {
            html += heading(tr("RF source"));
            for (const CaptureInfo::Row &item : from) html += row(item.name, item.value);
        }
    }

    if (ldx) {
        // What the LDX file holds: its captures, and what was done to it
        QVector<qint32> tracks;
        QStringList names;
        QString error;
        if (LdxCapture::listCaptures(path, tracks, names, error) && !tracks.isEmpty()) {
            html += heading(tr("Captures in the file"));
            for (int i = 0; i < tracks.size(); ++i) {
                html += row(tracks[i] == track ? tr("Track %1 (open)").arg(tracks[i]) : tr("Track %1").arg(tracks[i]), names.value(i));
            }
        }
        const QJsonArray history = LdxCapture::historyOf(file);
        if (!history.isEmpty()) {
            html += heading(tr("History"));
            for (const QJsonValue &value : history) {
                const QJsonObject entry = value.toObject();
                QString line = entry.value("command").toString();
                if (entry.contains("detail")) line += QStringLiteral(": ") + entry.value("detail").toString();
                const QDateTime when = QDateTime::fromString(entry.value("time").toString(), Qt::ISODate);
                html += row(when.isValid() ? when.toLocalTime().toString(Qt::ISODate).replace('T', ' ') : QString(), line);
            }
        }
    }
    html += QStringLiteral("</table>");
    return html;
}

SourceInfoDialog::SourceInfoDialog(QWidget *parent) : QWidget(parent)
{
    setMinimumWidth(260);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);

    // The cover of the file, if it is an LDX file that has one: whole, filling the width of the panel, in a frame that is sunk (the picture, or the one it points to;
    // its link opens when it is clicked)
    m_frame = new QFrame(this);
    m_frame->setFrameShape(QFrame::StyledPanel);
    m_frame->setFrameShadow(QFrame::Sunken);
    m_frame->setBackgroundRole(QPalette::Base);
    m_frame->setAutoFillBackground(true);
    auto *frameLayout = new QVBoxLayout(m_frame);
    frameLayout->setContentsMargins(0, 0, 0, 0);
    m_cover = new CoverWidget(m_frame);
    m_cover->setFill(true);
    frameLayout->addWidget(m_cover, 0, Qt::AlignHCenter);
    layout->addWidget(m_frame);
    m_frame->hide();

    m_text = new QTextBrowser(this);
    m_text->setObjectName(QStringLiteral("sourceText"));
    m_text->setOpenLinks(false);
    layout->addWidget(m_text, 1);
}

// What is known of the source that is open (nothing, when none is)
void SourceInfoDialog::setSource(TbcSource &source)
{
    m_text->setHtml(describe(source));
    m_cover->setContent(QByteArray(), QString(), QString(), QString());
    const QString file = source.getCurrentSourceFilename();
    if (LdxCapture::isLdxName(file)) {
        QString path;
        qint32 track = 0;
        LdxCapture::split(file, path, track);
        LdxCapture::CoverInfo info;
        QString error;
        if (LdxCapture::readCoverInfo(path, info, error)) m_cover->setContent(info.image, info.source, info.link, path);
    }
    m_frame->setVisible(!m_cover->isHidden());
    fitCover();
}

void SourceInfoDialog::clear()
{
    m_text->clear();
    m_cover->setContent(QByteArray(), QString(), QString(), QString());
    m_frame->hide();
}

void SourceInfoDialog::fitCover()
{
    m_cover->setRoom(QSize(std::max(120, width() - kCoverMargins), kMaxCoverHeight));
}

void SourceInfoDialog::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    fitCover();
}
