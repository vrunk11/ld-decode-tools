/******************************************************************************
 * coverwidget.cpp
 * ld-decode-tools - the cover of an LDX file, as the programs show it above what they know of the file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "coverwidget.h"

#include "coverimage.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#ifdef LDDECODE_WITH_NETWORK
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#endif

namespace {

constexpr qint64 kMaxFetched = 12 * 1024 * 1024;

// What was fetched in this session, by address: a picture is fetched once
QHash<QString, QByteArray> &fetched()
{
    static QHash<QString, QByteArray> cache;
    return cache;
}

bool isWeb(const QString &text)
{
    return text.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) || text.startsWith(QLatin1String("https://"), Qt::CaseInsensitive);
}

// The picture kept in a file: its path may be relative to the folder of the LDX file (and use / whatever the system), or be a file: address
QString localPath(const QString &source, const QString &ldxPath)
{
    QString text = source;
    if (text.startsWith(QLatin1String("file://"), Qt::CaseInsensitive)) text = QUrl(text).toLocalFile();
    if (QFileInfo(text).isAbsolute()) return QDir::cleanPath(text);
    return QDir::cleanPath(QFileInfo(ldxPath).absoluteDir().absoluteFilePath(text));
}

// A label that says it can be clicked
class ClickableLabel : public QLabel
{
public:
    std::function<void()> onClick;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && onClick) onClick();
        QLabel::mouseReleaseEvent(event);
    }
};

}

CoverWidget::CoverWidget(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *image = new ClickableLabel;
    image->setObjectName(QStringLiteral("coverPicture"));
    image->setAlignment(Qt::AlignCenter);
    image->onClick = [this] {
        if (m_picture.isNull()) return;
        // The link opens in the browser, when the picture is clicked; without one, the address of a picture on the web is what is opened
        const QString target = !m_link.isEmpty() ? m_link : (isWeb(m_source) ? m_source : QString());
        if (!target.isEmpty()) QDesktopServices::openUrl(QUrl(target));
        emit clicked();
    };
    m_image = image;
    layout->addWidget(m_image, 0, Qt::AlignHCenter);
    m_text = new QLabel;
    m_text->setObjectName(QStringLiteral("coverText"));
    m_text->setWordWrap(true);
    m_text->setAlignment(Qt::AlignCenter);
    m_text->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(m_text);
    m_fetch = new QPushButton(tr("Show the picture"));
    m_fetch->setObjectName(QStringLiteral("coverFetch"));
    layout->addWidget(m_fetch, 0, Qt::AlignHCenter);
    connect(m_fetch, &QPushButton::clicked, this, &CoverWidget::fetch);
    hide();
}

bool CoverWidget::webAvailable()
{
#ifdef LDDECODE_WITH_NETWORK
    return true;
#else
    return false;
#endif
}

void CoverWidget::setRoom(const QSize &room)
{
    m_room = room;
    fit();
}

void CoverWidget::show(const QPixmap &picture, const QString &text)
{
    m_picture = picture;
    m_text->setText(text);
    m_text->setVisible(!text.isEmpty());
    fit();
    setVisible(!m_picture.isNull() || !text.isEmpty());
}

void CoverWidget::fit()
{
    if (m_picture.isNull()) {
        m_image->clear();
        m_image->setFixedSize(0, 0);
        m_image->hide();
        return;
    }
    const QSize room(std::max(100, m_room.width()), m_room.height());
    const QPixmap shown = (m_fill || m_picture.width() > room.width() || m_picture.height() > room.height()) ? m_picture.scaled(room, Qt::KeepAspectRatio, Qt::SmoothTransformation) : m_picture;
    m_image->setPixmap(shown);
    m_image->setFixedSize(shown.size());
    const QString target = !m_link.isEmpty() ? m_link : (isWeb(m_source) ? m_source : QString());
    m_image->setCursor(target.isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
    m_image->setToolTip(target.isEmpty() ? tr("%1 × %2").arg(m_picture.width()).arg(m_picture.height()) : tr("Opens %1").arg(target));
    m_image->show();
}

void CoverWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    fit();
}

void CoverWidget::setContent(const QByteArray &image, const QString &source, const QString &link, const QString &ldxPath)
{
    m_link = isWeb(link) ? link : QString();
    m_source = source;
    m_fetch->hide();
    if (!image.isEmpty()) {
        show(QPixmap::fromImage(CoverImage::fromData(image)), QString());
        return;
    }
    if (source.isEmpty()) {
        show(QPixmap(), QString());
        return;
    }
    if (isWeb(source)) {
        // (a web address is not fetched by itself: what was fetched before is shown, else the address and a button)
        const auto cached = fetched().constFind(source);
        if (cached != fetched().constEnd()) {
            show(QPixmap::fromImage(CoverImage::fromData(cached.value())), QString());
            return;
        }
        show(QPixmap(), tr("The picture of the cover is at %1").arg(source));
        m_fetch->setVisible(webAvailable());
        return;
    }
    const QString path = localPath(source, ldxPath);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxFetched) {
        show(QPixmap(), tr("The picture of the cover is not there: %1").arg(QDir::toNativeSeparators(path)));
        return;
    }
    const QImage picture = CoverImage::fromData(file.readAll());
    show(QPixmap::fromImage(picture), picture.isNull() ? tr("The picture of the cover cannot be read: %1").arg(QDir::toNativeSeparators(path)) : QString());
}

// The picture on the web, fetched because the user asked for it: http or https, at most 12 MB, in a few seconds, once
void CoverWidget::fetch()
{
#ifdef LDDECODE_WITH_NETWORK
    const QString address = m_source;
    if (!isWeb(address)) return;
    m_fetch->setEnabled(false);
    m_text->setText(tr("Fetching %1 ...").arg(address));
    auto *manager = new QNetworkAccessManager(this);
    QNetworkRequest request{ QUrl(address) };
    request.setTransferTimeout(15000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setMaximumRedirectsAllowed(5);
    QNetworkReply *reply = manager->get(request);
    connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64) {
        if (received > kMaxFetched) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, manager, address] {
        const QByteArray data = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QString why = reply->errorString();
        reply->deleteLater();
        manager->deleteLater();
        m_fetch->setEnabled(true);
        if (address != m_source) return;        // (another file was opened meanwhile)
        const QImage picture = ok ? CoverImage::fromData(data) : QImage();
        if (picture.isNull()) {
            show(QPixmap(), ok ? tr("The picture of the cover cannot be read: %1").arg(address) : tr("The picture of the cover cannot be fetched: %1 (%2)").arg(address, why));
            m_fetch->setVisible(true);
            return;
        }
        fetched().insert(address, data);
        m_fetch->hide();
        show(QPixmap::fromImage(picture), QString());
    });
#endif
}
