/******************************************************************************
 * trackrow.cpp
 * ldx-packer - a line of the list of captures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "trackrow.h"

#include "format.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QString tagText(EntryState s)
{
    switch (s) {
    case EntryState::Kept: return QObject::tr("kept");
    case EntryState::Edited: return QObject::tr("edited");
    case EntryState::Recode: return QObject::tr("recode");
    case EntryState::New: return QObject::tr("new");
    case EntryState::Removed: return QObject::tr("removed");
    }
    return {};
}

QColor tagColor(EntryState s)
{
    switch (s) {
    case EntryState::Kept: return QColor(QStringLiteral("#9b9b9b"));
    case EntryState::Edited: return QColor(QStringLiteral("#378ADD"));
    case EntryState::Recode: return QColor(QStringLiteral("#BA7517"));
    case EntryState::New: return QColor(QStringLiteral("#1D9E75"));
    case EntryState::Removed: return QColor(QStringLiteral("#E24B4A"));
    }
    return {};
}

QToolButton *smallButton(const QString &text, const QString &tip, QWidget *parent)
{
    auto *b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(false);
    b->setFixedSize(22, 22);
    return b;
}

// PAL · 2 000 fields · 40.0 s · FLAC
QString detailsOf(const Entry &e)
{
    if (!e.fromArchive && e.ldxFile.isEmpty()) return QObject::tr("to pack");
    const bool cvbs = e.video.kind == Ldx::Kind::Cvbs;
    QStringList parts;
    const QString system = e.video.parameters.value("system").toString();
    if (!system.isEmpty()) parts << system;
    parts << QObject::tr("%1 %2").arg(fmt::count(e.blocks), cvbs ? QObject::tr("frames") : QObject::tr("fields"));
    if (e.seconds > 0.0) parts << fmt::duration(e.seconds);
    parts << Ldx::codecName(e.video.codec).toUpper();
    if (!e.ldxFile.isEmpty()) parts.prepend(QObject::tr("from %1").arg(QFileInfo(e.ldxFile).fileName()));
    return parts.join(QStringLiteral(" · "));
}

} // namespace

TrackRow::TrackRow(const Entry &entry, int index, EntryState state, const QColor &color, bool picked, bool canUp, bool canDown, bool expandable, bool expanded, QWidget *parent)
    : QWidget(parent), m_index(index), m_color(color), m_picked(picked), m_faded(state == EntryState::Removed)
{
    setObjectName(QStringLiteral("trackRow"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 0, 0, 0);
    outer->setSpacing(0);

    auto *top = new QHBoxLayout;
    top->setContentsMargins(0, 6, 6, 6);
    top->setSpacing(8);
    auto *fold = smallButton(expanded ? QStringLiteral("▾") : QStringLiteral("▸"), expanded ? tr("Hide the tracks that go with it") : tr("Show the tracks that go with it"), this);
    fold->setObjectName(QStringLiteral("rowFold"));
    fold->setEnabled(expandable);
    fold->setVisible(expandable);
    top->addWidget(fold);
    connect(fold, &QToolButton::clicked, this, [this] { emit expandToggled(m_index); });
    auto *number = new QLabel(QString::number(index), this);
    number->setMinimumWidth(18);
    number->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    number->setForegroundRole(QPalette::PlaceholderText);
    top->addWidget(number);

    auto *name = new QLabel(this);
    name->setTextFormat(Qt::RichText);
    const QString shown = entry.name.toHtmlEscaped();
    const QString struck = m_faded ? QStringLiteral("<s>%1</s>").arg(shown) : shown;
    name->setText(QStringLiteral("%1<br><small>%2</small>").arg(struck, detailsOf(entry).toHtmlEscaped()));
    name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    if (m_faded) name->setEnabled(false);
    top->addWidget(name, 1);

    auto *tag = new QLabel(tagText(state), this);
    tag->setObjectName(QStringLiteral("rowTag"));
    const QColor c = tagColor(state);
    tag->setStyleSheet(QStringLiteral("QLabel { color: %1; border: 1px solid %1; padding: 0 5px; }").arg(c.name()));
    top->addWidget(tag);

    auto *up = smallButton(QStringLiteral("▲"), tr("Move up"), this);
    up->setObjectName(QStringLiteral("rowUp"));
    up->setEnabled(canUp);
    auto *down = smallButton(QStringLiteral("▼"), tr("Move down"), this);
    down->setObjectName(QStringLiteral("rowDown"));
    down->setEnabled(canDown);
    const bool removed = state == EntryState::Removed;
    auto *drop = smallButton(removed ? QStringLiteral("↩") : QStringLiteral("✕"), removed ? tr("Keep the capture") : tr("Remove the capture"), this);
    drop->setObjectName(QStringLiteral("rowRemove"));
    top->addWidget(up);
    top->addWidget(down);
    top->addWidget(drop);
    outer->addLayout(top);

    if (up) connect(up, &QToolButton::clicked, this, [this] { emit moved(m_index, -1); });
    if (down) connect(down, &QToolButton::clicked, this, [this] { emit moved(m_index, 1); });
    connect(drop, &QToolButton::clicked, this, [this, removed] { removed ? emit restoreAsked(m_index) : emit removeAsked(m_index); });
}

void TrackRow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    QColor tint = m_color;
    tint.setAlphaF(m_faded ? 0.07 : (m_picked ? 0.40 : 0.20));
    p.fillRect(rect(), tint);
    p.fillRect(QRect(0, 0, m_picked ? 7 : 5, height()), m_color);
    QColor line = palette().color(QPalette::Text);
    line.setAlpha(90);
    p.setPen(line);
    p.drawLine(0, height() - 1, width(), height() - 1);
}

void TrackRow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) emit picked(m_index);
    QWidget::mousePressEvent(event);
}

// ---------------------------------------------------------------------------------------------------------------
// A track that goes with a capture

namespace {

// What a track is, in a few words for its line: the kind, the codec of samples, the rate of audio
QString childDetails(const Companion &c)
{
    QStringList parts;
    if (c.isNew()) parts << QObject::tr("to add");
    if (c.depth > 0) parts << QObject::tr("%1 bits").arg(c.depth);
    if (c.sampleRate > 0.0) parts << QObject::tr("%1 Hz").arg(c.sampleRate, 0, 'f', 0);
    if (c.rateDiffers() && !c.declareRate) parts << QObject::tr("header says %1 Hz").arg(c.headerRate, 0, 'f', 0);
    if (!c.isNew() && isSamples(c.kind)) parts << Ldx::codecName(c.codec).toUpper();
    if (!c.detail.isEmpty()) parts << c.detail;
    return parts.join(QStringLiteral(" · "));
}

}

TrackChildRow::TrackChildRow(const Companion &track, int capture, int row, const QColor &color, bool picked, QWidget *parent)
    : QWidget(parent), m_capture(capture), m_row(row), m_color(color), m_picked(picked), m_faded(track.removed)
{
    build(track.chroma ? QObject::tr("chroma") : Ldx::kindName(track.kind), track.name, childDetails(track), track.removed);
}

TrackChildRow::TrackChildRow(const QString &notes, int capture, const QColor &color, bool picked, QWidget *parent)
    : QWidget(parent), m_capture(capture), m_row(-1), m_color(color), m_picked(picked)
{
    QString first = notes.section(QLatin1Char('\n'), 0, 0);
    if (first.size() > 60) first = first.left(60) + QStringLiteral("…");
    build(QObject::tr("notes"), first, QString(), false);
}

void TrackChildRow::build(const QString &kind, const QString &text, const QString &details, bool faded)
{
    setObjectName(QStringLiteral("trackChildRow"));
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(46, 6, 8, 6);
    layout->setSpacing(8);
    auto *tag = new QLabel(kind, this);
    tag->setMinimumWidth(70);
    tag->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(tag);
    auto *label = new QLabel(this);
    label->setTextFormat(Qt::RichText);
    const QString shown = text.toHtmlEscaped();
    label->setText(details.isEmpty() ? (faded ? QStringLiteral("<s>%1</s>").arg(shown) : shown)
                                     : QStringLiteral("%1 <small>%2</small>").arg(faded ? QStringLiteral("<s>%1</s>").arg(shown) : shown, details.toHtmlEscaped()));
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    if (faded) label->setEnabled(false);
    layout->addWidget(label, 1);
    if (m_row >= 0) {
        auto *drop = smallButton(m_faded ? QStringLiteral("↩") : QStringLiteral("✕"), m_faded ? tr("Keep the track") : tr("Take the track out of the file"), this);
        drop->setObjectName(QStringLiteral("childRemove"));
        layout->addWidget(drop);
        connect(drop, &QToolButton::clicked, this, [this] { emit removeAsked(m_capture, m_row); });
    }
}

void TrackChildRow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    QColor tint = m_color;
    tint.setAlphaF(m_faded ? 0.04 : (m_picked ? 0.30 : 0.09));
    p.fillRect(rect(), tint);
    p.fillRect(QRect(0, 0, m_picked ? 5 : 3, height()), m_color);
    QColor line = palette().color(QPalette::Text);
    line.setAlpha(70);
    p.setPen(line);
    p.drawLine(12, height() - 1, width(), height() - 1);
}

void TrackChildRow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) emit picked(m_capture, m_row);
    QWidget::mousePressEvent(event);
}
