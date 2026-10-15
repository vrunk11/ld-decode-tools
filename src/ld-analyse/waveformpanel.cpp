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

WaveformView::WaveformView(AudioEngine *engine, QWidget *parent) : QWidget(parent), engine(engine), colour(QColor(0x2a, 0x82, 0xda))
{
    setMinimumHeight(48);
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
    painter.fillRect(rect(), palette().color(QPalette::Base));

    const qint32 w = width(), h = height();
    const qint64 total = engine->samples();
    const qint32 middle = h / 2;
    painter.setPen(palette().color(QPalette::Mid));
    painter.drawLine(0, middle, w, middle);
    if (total < 1) return;

    // One column for a run of the audio: the highest and lowest value of each channel over it, the left channel above the
    // middle and the right below, so that each has a half of the height
    const AudioPeaks &peaks = engine->peaks();
    const qint64 available = peaks.samples();
    const double half = h / 4.0;
    QColor faint = colour;
    faint.setAlpha(150);
    painter.setPen(faint);
    for (qint32 x = 0; x < w; x++) {
        const qint64 first = total * x / w;
        const qint64 last = qMax(first + 1, total * (x + 1) / w);
        if (first >= available) break;
        const AudioPeaks::Peak peak = peaks.range(first, qMin(last, available));
        const double leftTop = (h / 4.0) - (peak.maxLeft / 32768.0) * half;
        const double leftBottom = (h / 4.0) - (peak.minLeft / 32768.0) * half;
        const double rightTop = (3.0 * h / 4.0) - (peak.maxRight / 32768.0) * half;
        const double rightBottom = (3.0 * h / 4.0) - (peak.minRight / 32768.0) * half;
        painter.drawLine(QPointF(x + 0.5, leftTop), QPointF(x + 0.5, qMax(leftBottom, leftTop + 1.0)));
        painter.drawLine(QPointF(x + 0.5, rightTop), QPointF(x + 0.5, qMax(rightBottom, rightTop + 1.0)));
    }

    // Where the video is
    const qint64 sample = engine->sampleAt(playhead);
    if (sample >= 0 && sample <= total) {
        const qint32 x = static_cast<qint32>(static_cast<double>(sample) * w / static_cast<double>(total));
        painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
        painter.drawLine(x, 0, x, h);
    }
}

// ---------------------------------------------------------------------------------------------------------------------

WaveformPanel::WaveformPanel(AudioEngine *engine, QWidget *parent) : QWidget(parent)
{
    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    setMaximumHeight(72);

    controls = new QWidget(this);
    controls->setFixedWidth(170);
    QVBoxLayout *column = new QVBoxLayout(controls);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(3);

    sourceCombo = new QComboBox(controls);
    sourceCombo->setToolTip(tr("Which audio is played: analogue or decoded from EFM"));
    trackCombo = new QComboBox(controls);
    trackCombo->setToolTip(tr("Which track of the file"));
    column->addWidget(sourceCombo);
    column->addWidget(trackCombo);

    // The channels and the volume on one line
    QHBoxLayout *mixRow = new QHBoxLayout();
    mixRow->setSpacing(3);
    leftButton = new QToolButton(controls);
    leftButton->setText(tr("L"));
    leftButton->setCheckable(true);
    leftButton->setChecked(true);
    leftButton->setAutoRaise(true);
    leftButton->setToolTip(tr("Play the left channel"));
    rightButton = new QToolButton(controls);
    rightButton->setText(tr("R"));
    rightButton->setCheckable(true);
    rightButton->setChecked(true);
    rightButton->setAutoRaise(true);
    rightButton->setToolTip(tr("Play the right channel"));
    volumeSlider = new QSlider(Qt::Horizontal, controls);
    volumeSlider->setRange(0, 100);
    volumeSlider->setValue(70);
    volumeSlider->setToolTip(tr("Volume"));
    mixRow->addWidget(leftButton);
    mixRow->addWidget(rightButton);
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
    connect(volumeSlider, &QSlider::valueChanged, this, &WaveformPanel::emitMix);
    connect(view, &WaveformView::seekRequested, this, &WaveformPanel::seekRequested);
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

bool WaveformPanel::leftOn() const { return leftButton->isChecked(); }
bool WaveformPanel::rightOn() const { return rightButton->isChecked(); }
qint32 WaveformPanel::volume() const { return volumeSlider->value(); }

void WaveformPanel::setMix(bool left, bool right, qint32 volume)
{
    const QSignalBlocker a(leftButton), b(rightButton), c(volumeSlider);
    leftButton->setChecked(left);
    rightButton->setChecked(right);
    volumeSlider->setValue(volume);
}

void WaveformPanel::emitMix()
{
    emit mixChanged(leftButton->isChecked(), rightButton->isChecked(), volumeSlider->value());
}
