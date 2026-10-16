/******************************************************************************
 * waveformpanel.cpp
 * ld-analyse - the waveform of the audio, with the controls of what is played
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "waveformpanel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QSlider>
#include <QStandardItemModel>
#include <QToolButton>
#include <QVBoxLayout>

#include "audioengine.h"
#include "theme.h"

WaveformView::WaveformView(AudioEngine *engine, QWidget *parent) : QWidget(parent), engine(engine), colour()
{
    setMouseTracking(false);
    setToolTip(tr("Click to go to a place in the audio"));
}

void WaveformView::setPlayheadSeconds(double videoSeconds)
{
    if (videoSeconds == playhead) return;
    playhead = videoSeconds;
    update();
}

void WaveformView::setColor(const QColor &newColour)
{
    colour = newColour;
    update();
}

double WaveformView::secondsAtX(qint32 x) const
{
    const qint64 total = engine->samples();
    if (total < 1 || width() < 1) return 0.0;
    const double fraction = qBound(0.0, static_cast<double>(x) / width(), 1.0);
    return engine->videoSecondsOf(static_cast<qint64>(fraction * static_cast<double>(total)));
}

void WaveformView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && engine->samples() > 0) emit seekRequested(secondsAtX(event->position().toPoint().x()));
}

void WaveformView::mouseMoveEvent(QMouseEvent *event)
{
    if ((event->buttons() & Qt::LeftButton) && engine->samples() > 0) emit seekRequested(secondsAtX(event->position().toPoint().x()));
}

void WaveformView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const qint32 w = width(), h = height();
    const qint64 total = engine->samples();
    const qint32 middle = h / 2;
    // (no background of its own: the application's, with a faint grid as the SNR graphs have, that shows on any theme)
    QColor grid = palette().color(QPalette::Text);
    grid.setAlpha(70);
    painter.setPen(QPen(grid, 1));
    painter.drawLine(0, middle, w, middle);
    painter.drawLine(0, h / 4, w, h / 4);
    painter.drawLine(0, 3 * h / 4, w, 3 * h / 4);
    if (total < 1) {
        if (!message.isEmpty()) {
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(rect().adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap, message);
        }
        return;
    }

    // One column for a run of the audio: the highest and lowest value of each channel over it, the left channel above the
    // middle and the right below, so that each has a half of the height
    const AudioPeaks &peaks = engine->peaks();
    const qint64 available = peaks.samples();
    const double half = h / 4.0;
    // (the colour of the theme, for the kind of audio that is played, unless one was given)
    const qint32 chosen = engine->currentTrack();
    const bool fromEfm = chosen >= 0 && chosen < engine->tracks().size() && engine->tracks()[chosen].source == "efm";
    const QColor base = colour.isValid() ? colour : waveformColour(fromEfm);
    QColor faint = base;
    faint.setAlpha(style == Style::Filled ? 170 : 255);
    painter.setPen(faint);
    const qint32 step = style == Style::Bars ? 3 : 1;
    double previousTop[2] = { -1, -1 }, previousBottom[2] = { -1, -1 };
    for (qint32 x = 0; x < w; x += step) {
        const qint64 first = total * x / w;
        const qint64 last = qMax(first + 1, total * (x + 1) / w);
        if (first >= available) break;
        const AudioPeaks::Peak peak = peaks.range(first, qMin(last, available));
        const double leftTop = (h / 4.0) - (peak.maxLeft / 32768.0) * half;
        const double leftBottom = (h / 4.0) - (peak.minLeft / 32768.0) * half;
        const double rightTop = (3.0 * h / 4.0) - (peak.maxRight / 32768.0) * half;
        const double rightBottom = (3.0 * h / 4.0) - (peak.minRight / 32768.0) * half;
        const double tops[2] = { leftTop, rightTop }, bottoms[2] = { qMax(leftBottom, leftTop + 1.0), qMax(rightBottom, rightTop + 1.0) };
        for (qint32 c = 0; c < 2; c++) {
            if (style == Style::Outline) {
                // the edge of the shape only: a line along the highest and along the lowest values
                if (previousTop[c] >= 0) {
                    painter.drawLine(QPointF(x - 0.5, previousTop[c]), QPointF(x + 0.5, tops[c]));
                    painter.drawLine(QPointF(x - 0.5, previousBottom[c]), QPointF(x + 0.5, bottoms[c]));
                }
                previousTop[c] = tops[c];
                previousBottom[c] = bottoms[c];
            } else if (style == Style::Bars) {
                painter.fillRect(QRectF(x, tops[c], 2.0, bottoms[c] - tops[c]), faint);
            } else {
                painter.drawLine(QPointF(x + 0.5, tops[c]), QPointF(x + 0.5, bottoms[c]));
            }
        }
    }

    // Where the video is
    const qint64 sample = engine->sampleAt(playhead);
    if (sample >= 0 && sample <= total) {
        const qint32 x = static_cast<qint32>(static_cast<double>(sample) * w / static_cast<double>(total));
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
        painter.drawLine(x, 0, x, h);
    }
}

// ---------------------------------------------------------------------------------------------------------------------

WaveformPanel::WaveformPanel(AudioEngine *engine, QWidget *parent) : QFrame(parent)
{
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Sunken);
    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(6);

    controls = new QWidget(this);
    controls->setFixedWidth(170);
    column = new QVBoxLayout(controls);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(3);

    sourceCombo = new QComboBox(controls);
    sourceCombo->setToolTip(tr("Which audio is played: analogue or decoded from EFM"));
    trackCombo = new QComboBox(controls);
    trackCombo->setToolTip(tr("Which track of the file"));
    column->addWidget(sourceCombo);
    column->addWidget(trackCombo);

    // The channels and the volume on one line
    mixRow = new QHBoxLayout();
    mixRow->setSpacing(3);
    leftButton = new QToolButton(controls);
    leftButton->setText(tr("L"));
    leftButton->setCheckable(true);
    leftButton->setChecked(false);
    
    leftButton->setToolTip(tr("Mute the left channel"));
    rightButton = new QToolButton(controls);
    rightButton->setText(tr("R"));
    rightButton->setCheckable(true);
    rightButton->setChecked(false);
    
    rightButton->setToolTip(tr("Mute the right channel"));
    muteButton = new QToolButton(controls);
    muteButton->setText(tr("M"));
    muteButton->setCheckable(true);
    
    muteButton->setToolTip(tr("Mute"));
    volumeSlider = new QSlider(Qt::Horizontal, controls);
    volumeSlider->setRange(0, 100);
    volumeSlider->setValue(70);
    volumeSlider->setToolTip(tr("Volume"));
    mixRow->addWidget(leftButton);
    mixRow->addWidget(rightButton);
    mixRow->addWidget(muteButton);
    mixRow->addWidget(volumeSlider, 1);
    column->addLayout(mixRow);
    column->addStretch(1);

    view = new WaveformView(engine, this);
    layout->addWidget(controls);
    layout->addWidget(view, 1);

    connect(sourceCombo, &QComboBox::currentIndexChanged, this, [this]() { if (!filling) sourceChosen(); });
    connect(trackCombo, &QComboBox::currentIndexChanged, this, [this]() {
        if (filling || trackCombo->currentIndex() < 0) return;
        emit trackSelected(trackCombo->currentData().toInt());
    });
    connect(leftButton, &QToolButton::toggled, this, &WaveformPanel::emitMix);
    connect(rightButton, &QToolButton::toggled, this, &WaveformPanel::emitMix);
    connect(muteButton, &QToolButton::toggled, this, &WaveformPanel::mutedChanged);
    connect(volumeSlider, &QSlider::valueChanged, this, &WaveformPanel::emitMix);
    connect(view, &WaveformView::seekRequested, this, &WaveformPanel::seekRequested);
    setDensity(Density::Compact);
}

void WaveformPanel::setTracks(const QVector<AudioTrackInfo> &tracks, qint32 chosen)
{
    filling = true;
    allTracks = tracks;
    sourceCombo->clear();
    const QList<QPair<QString, QString>> sources { { "analogue", tr("Analogue") }, { "efm", tr("EFM digital") }, { "import", tr("Imported") } };
    QStringList seen;
    for (const AudioTrackInfo &track: tracks) {
        if (seen.contains(track.source)) continue;
        seen.append(track.source);
        QString name = track.source;
        for (const auto &known: sources) if (known.first == track.source) name = known.second;
        sourceCombo->addItem(name, track.source);
    }
    // The sources that are not there are shown, out of reach, so that the way to get them is in sight
    for (const auto &known: sources) {
        if (known.first == "import" || seen.contains(known.first)) continue;
        sourceCombo->addItem(tr("%1 (none)").arg(known.second), known.first);
        if (auto *model = qobject_cast<QStandardItemModel *>(sourceCombo->model())) {
            model->item(sourceCombo->count() - 1)->setEnabled(false);
        }
    }
    // The source of the track that is chosen, and the tracks of that source
    const QString wanted = (chosen >= 0 && chosen < tracks.size()) ? tracks[chosen].source : QString();
    const qint32 sourceIndex = sourceCombo->findData(wanted);
    if (sourceIndex >= 0) sourceCombo->setCurrentIndex(sourceIndex);
    filling = false;
    sourceChosen();
    if (chosen >= 0) {
        filling = true;
        trackCombo->setCurrentIndex(trackCombo->findData(chosen));
        filling = false;
    }
}

// The tracks of the source that is chosen fill the other choice
void WaveformPanel::sourceChosen()
{
    const QString source = sourceCombo->currentData().toString();
    const bool wasFilling = filling;
    filling = true;
    trackCombo->clear();
    for (qint32 i = 0; i < allTracks.size(); i++) {
        if (allTracks[i].source == source) trackCombo->addItem(allTracks[i].label, i);
    }
    filling = wasFilling;
    // One choice only is not a choice: it is not shown
    trackCombo->setVisible(trackCombo->count() > 1);
    if (!filling && trackCombo->count() > 0) emit trackSelected(trackCombo->itemData(0).toInt());
}

void WaveformPanel::setControlsVisible(bool visible)
{
    controls->setVisible(visible);
}

bool WaveformPanel::leftOn() const { return !leftButton->isChecked(); }
bool WaveformPanel::rightOn() const { return !rightButton->isChecked(); }
qint32 WaveformPanel::volume() const { return volumeSlider->value(); }

void WaveformPanel::setMix(bool left, bool right, qint32 volume)
{
    const QSignalBlocker a(leftButton), b(rightButton), c(volumeSlider);
    leftButton->setChecked(!left);
    rightButton->setChecked(!right);
    volumeSlider->setValue(volume);
}

void WaveformPanel::emitMix()
{
    emit mixChanged(!leftButton->isChecked(), !rightButton->isChecked(), volumeSlider->value());
}

bool WaveformPanel::muted() const { return muteButton->isChecked(); }

void WaveformPanel::setMuted(bool muted)
{
    const QSignalBlocker blocker(muteButton);
    muteButton->setChecked(muted);
}

void WaveformPanel::setDensity(Density density)
{
    const bool ultra = density == Density::Ultra;
    column->setDirection(ultra ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    controls->setFixedWidth(ultra ? 380 : 170);
    sourceCombo->setMaximumWidth(ultra ? 130 : QWIDGETSIZE_MAX);
    trackCombo->setMaximumWidth(ultra ? 110 : QWIDGETSIZE_MAX);
    volumeSlider->setMinimumWidth(ultra ? 80 : 0);
    const qint32 viewHeight = density == Density::Large ? 72 : (density == Density::Compact ? 40 : 20);
    view->setMinimumHeight(viewHeight);
    const qint32 margin = ultra ? 2 : 4;
    layout()->setContentsMargins(margin, margin, margin, margin);
    // (the panel is as high as its controls need, or as the waveform asks, whichever is more, and not much more)
    setMaximumHeight(density == Density::Large ? 110 : (density == Density::Compact ? 60 : 32));
    updateGeometry();
}
