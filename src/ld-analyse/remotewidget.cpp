/******************************************************************************
 * remotewidget.cpp
 * ld-analyse - the remote control of a laserdisc player, to drive the playback of a capture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "remotewidget.h"

#include "jogwheel.h"

#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

// The look of the remote is its own (a dark body with grey and dark keys, an amber display), whatever the theme of the program is
static const char *REMOTE_STYLE =
    "RemoteWidget { background: #26262a; }"
    "RemoteWidget QPushButton { background: #8c8c92; color: #101012; border: 1px solid #141416; border-radius: 3px; padding: 2px 3px;"
    "  font-size: 9px; font-weight: 600; min-height: 20px; }"
    "RemoteWidget QPushButton[role=\"num\"] { background: #3c3c42; color: #ececee; font-size: 13px; }"
    "RemoteWidget QPushButton[role=\"wide\"] { background: #3c3c42; color: #ececee; }"
    "RemoteWidget QPushButton[role=\"play\"] { background: #5c5c62; color: #ececee; font-size: 12px; }"
    "RemoteWidget QPushButton[role=\"mode\"] { background: transparent; color: #d9a441; border: none; font-size: 9px; text-align: left; padding: 0 2px; }"
    "RemoteWidget QPushButton[role=\"side\"] { background: #2f5d9c; color: #f0f0f4; }"
    "RemoteWidget QPushButton:pressed { background: #b8b8be; color: #101012; }"
    "RemoteWidget QPushButton[lit=\"true\"] { background: #e8a020; color: #101012; }"
    "RemoteWidget QPushButton:disabled { background: #48484c; color: #7c7c82; }"
    "RemoteWidget QLabel { color: #b8b8be; font-size: 8px; }"
    "RemoteWidget QLabel#entry { color: #f2b84a; font-family: monospace; font-size: 22px; font-weight: 700; }"
    "RemoteWidget QLabel#state { color: #d9a441; font-size: 10px; }"
    "RemoteWidget QFrame#lcd { background: #0c0c0e; border: 1px solid #000; border-radius: 3px; }";

QPushButton *RemoteWidget::key(const QString &text, const QString &tip, const QString &role)
{
    QPushButton *button = new QPushButton(text, this);
    // (the keys must not take the focus: the keyboard goes on being the player's)
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolTip(tip);
    button->setProperty("role", role);
    button->setAutoRepeat(false);
    return button;
}

void RemoteWidget::setLit(QPushButton *button, bool lit)
{
    if (button->property("lit").toBool() == lit) return;
    button->setProperty("lit", lit);
    button->style()->unpolish(button);
    button->style()->polish(button);
}

RemoteWidget::RemoteWidget(QWidget *parent) : QWidget(parent)
{
    setObjectName("remoteWidget");
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QString::fromLatin1(REMOTE_STYLE));
    setFocusPolicy(Qt::ClickFocus);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 10);
    layout->setSpacing(5);

    // The top row: the repeat between two points, CX and the mute (the other keys made here are put in their places further down)
    {
        QHBoxLayout *row = new QHBoxLayout();
        row->setSpacing(4);
        QLabel *repeatLabel = new QLabel(tr("REPEAT"), this);
        repeatA = key("A", tr("Mark the start of a part to repeat (a second press clears it)"), "fn");
        repeatB = key("B", tr("Mark the end: the part between A and B plays again and again"), "fn");
        cxButton = key("CX", tr("The disc says it has CX noise reduction (the lamp only: it is not decoded here)"), "fn");
        cxButton->setEnabled(false);
        digitalAnalogButton = key(tr("D/A"), tr("Digital / analogue: the sound that is heard, decoded from the EFM (lit, DIG on the display) or the analogue one (ANA)"), "fn");
        chromaButton = key(tr("RAW"), tr("Raw: the picture as it was captured, without the colour decoded (lit), or the colour decoded"), "fn");
        muteButton = key(tr("MUTE"), tr("The sound off (lit): the waveform panel has the same"), "fn");
        viewButton = key(tr("FRAM"), tr("How the picture is shown: frame (FRAM), split (SPLT), field (FLD), deinterlaced (DEIN); a press goes to the next"), "fn");
        row->addWidget(repeatLabel);
        row->addWidget(repeatA);
        row->addWidget(repeatB);
        row->addSpacing(6);
        row->addWidget(cxButton);
        row->addWidget(muteButton, 1);
        layout->addLayout(row);
        connect(repeatA, &QPushButton::clicked, this, &RemoteWidget::repeatAPressed);
        connect(repeatB, &QPushButton::clicked, this, &RemoteWidget::repeatBPressed);
        connect(digitalAnalogButton, &QPushButton::clicked, this, &RemoteWidget::digitalAnalogPressed);
        connect(chromaButton, &QPushButton::clicked, this, &RemoteWidget::chromaPressed);
        connect(muteButton, &QPushButton::clicked, this, &RemoteWidget::mutePressed);
        connect(viewButton, &QPushButton::clicked, this, &RemoteWidget::viewModePressed);
        needsSource << repeatA << repeatB << chromaButton << viewButton;
    }

    // The display: how a search is made (a key to change it), the number being typed, and what the player is doing
    {
        QFrame *lcd = new QFrame(this);
        lcd->setObjectName("lcd");
        QVBoxLayout *inside = new QVBoxLayout(lcd);
        inside->setContentsMargins(6, 3, 6, 3);
        inside->setSpacing(0);
        modeButton = key(QString(), tr("What the number is for: a chapter, a picture, a time or a frame (a press changes it)"), "mode");
        entryLabel = new QLabel("_", lcd);
        entryLabel->setObjectName("entry");
        entryLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        stateLabel = new QLabel(lcd);
        stateLabel->setObjectName("state");
        stateLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        audioLabel = new QLabel(lcd);
        audioLabel->setObjectName("state");
        audioLabel->setToolTip(tr("The sound that is heard: analogue (ANA) or digital, decoded from the EFM (DIG)"));
        QHBoxLayout *top = new QHBoxLayout();
        top->addWidget(modeButton);
        top->addStretch(1);
        top->addWidget(audioLabel);
        top->addSpacing(8);
        top->addWidget(stateLabel);
        inside->addLayout(top);
        inside->addWidget(entryLabel);
        layout->addWidget(lcd);
        connect(modeButton, &QPushButton::clicked, this, &RemoteWidget::modePressed);
        needsSource << modeButton;
    }

    // The keys: the numbers, and on their left the keys that do something with them
    {
        QGridLayout *grid = new QGridLayout();
        grid->setSpacing(4);
        QPushButton *program = key("PGM", tr("A list of the chapters and their times, to go to one of them"), "fn");
        connect(program, &QPushButton::clicked, this, &RemoteWidget::programPressed);
        needsSource << program;
        memoryButton = key(tr("ONE-SHOT\nMEMORY"), tr("Remember the place; the next press goes back to it"), "fn");
        stillButton = key(tr("STILL WITH\nSOUND"), tr("The picture stays and the sound goes on"), "fn");
        filterButton = key(tr("FILTER"), tr("The ffmpeg video filters (Window > Video filter...) on or off"), "fn");
        connect(filterButton, &QPushButton::clicked, this, &RemoteWidget::filterPressed);
        grid->addWidget(program, 0, 0);
        grid->addWidget(memoryButton, 1, 0);
        grid->addWidget(viewButton, 2, 0);
        grid->addWidget(stillButton, 3, 0, 1, 3);
        grid->addWidget(filterButton, 3, 3, 1, 2);
        for (qint32 digit = 0; digit < 10; digit++) {
            QPushButton *button = key(QString::number(digit), tr("The digit %1 of a number to search").arg(digit), "num");
            button->setMinimumHeight(26);
            // (the 0 is at the end of the third row, the others are in rows of three)
            grid->addWidget(button, digit == 0 ? 2 : (digit - 1) / 3, digit == 0 ? 4 : (digit - 1) % 3 + 1);
            connect(button, &QPushButton::clicked, this, [this, digit]() { emit digitPressed(digit); });
            needsSource << button;
        }
        QPushButton *clear = key("C", tr("Clear the number"), "num");
        clear->setMinimumHeight(26);
        QPushButton *plusTen = key("+10", tr("Ten more"), "num");
        plusTen->setMinimumHeight(26);
        grid->addWidget(clear, 0, 4);
        grid->addWidget(plusTen, 1, 4);
        connect(clear, &QPushButton::clicked, this, &RemoteWidget::clearPressed);
        connect(plusTen, &QPushButton::clicked, this, &RemoteWidget::plusTenPressed);
        connect(memoryButton, &QPushButton::clicked, this, &RemoteWidget::memoryPressed);
        connect(stillButton, &QPushButton::clicked, this, &RemoteWidget::stillWithSoundPressed);
        needsSource << clear << plusTen << memoryButton << stillButton;
        layout->addLayout(grid);
    }

    // The multi-speed (slower, faster, and the way) and the search, the step
    {
        QGridLayout *grid = new QGridLayout();
        grid->setSpacing(4);
        QLabel *multi = new QLabel(tr("MULTI-SPEED"), this);
        multi->setAlignment(Qt::AlignCenter);
        QPushButton *slower = key(QStringLiteral("−"), tr("Slower"), "wide");
        QPushButton *faster = key(QStringLiteral("+"), tr("Faster"), "wide");
        QPushButton *reverse = key(QStringLiteral("◀"), tr("Go backward at that speed"), "wide");
        QPushButton *forward = key(QStringLiteral("▶"), tr("Go forward at that speed"), "wide");
        QLabel *searchLabel = new QLabel(tr("SEARCH"), this);
        searchLabel->setAlignment(Qt::AlignCenter);
        QPushButton *search = key(tr("CHP/TM"), tr("Search: go to the chapter, the picture or the time that was typed (the mode on the display says which)"), "wide");
        QPushButton *frameSearch = key(tr("FRAME"), tr("Search: go to the frame of the capture that was typed"), "wide");
        QLabel *stepLabel = new QLabel(tr("STEP"), this);
        stepLabel->setAlignment(Qt::AlignCenter);
        QPushButton *stepBack = key(QStringLiteral("◀II"), tr("One picture back"), "wide");
        QPushButton *stepForward = key(QStringLiteral("II▶"), tr("One picture on"), "wide");
        grid->addWidget(multi, 0, 0, 1, 2);
        grid->addWidget(slower, 1, 0);
        grid->addWidget(faster, 1, 1);
        grid->addWidget(reverse, 2, 0);
        grid->addWidget(forward, 2, 1);
        grid->addWidget(searchLabel, 0, 2);
        grid->addWidget(search, 1, 2);
        grid->addWidget(frameSearch, 2, 2);
        grid->addWidget(stepLabel, 0, 3, 1, 2);
        grid->addWidget(stepBack, 1, 3);
        grid->addWidget(stepForward, 1, 4);
        grid->addWidget(digitalAnalogButton, 2, 3);
        grid->addWidget(chromaButton, 2, 4);
        connect(slower, &QPushButton::clicked, this, [this]() { emit multiSpeedPressed(-1); });
        connect(faster, &QPushButton::clicked, this, [this]() { emit multiSpeedPressed(1); });
        connect(reverse, &QPushButton::clicked, this, [this]() { emit directionPressed(-1); });
        connect(forward, &QPushButton::clicked, this, [this]() { emit directionPressed(1); });
        connect(search, &QPushButton::clicked, this, &RemoteWidget::searchPressed);
        connect(frameSearch, &QPushButton::clicked, this, &RemoteWidget::frameSearchPressed);
        connect(stepBack, &QPushButton::clicked, this, [this]() { emit stepPressed(-1); });
        connect(stepForward, &QPushButton::clicked, this, [this]() { emit stepPressed(1); });
        stepBack->setAutoRepeat(true);
        stepForward->setAutoRepeat(true);
        needsSource << slower << faster << reverse << forward << search << frameSearch << stepBack << stepForward;
        layout->addLayout(grid);
    }

    // Skip, play, skip
    {
        QHBoxLayout *row = new QHBoxLayout();
        row->setSpacing(4);
        QLabel *skip = new QLabel(tr("SKIP"), this);
        QPushButton *skipBack = key(QStringLiteral("|<"), tr("The start of the chapter, or the one before"), "play");
        playButton = key(QStringLiteral("▶"), tr("Play at the normal speed"), "play");
        QPushButton *skipForward = key(QStringLiteral(">|"), tr("The start of the next chapter"), "play");
        // (the icons of the buttons of the main window; the text is for where the program has not its resources)
        for (QPushButton *button: { skipBack, skipForward }) {
            const QString iconName = button == skipBack ? QStringLiteral(":/icons/Graphics/start-frame.svg") : QStringLiteral(":/icons/Graphics/end-frame.svg");
            if (!QFile::exists(iconName)) continue;
            button->setIcon(QIcon(iconName));
            button->setIconSize(QSize(24, 24));
            button->setText(QString());
        }
        skipBack->setMinimumHeight(28);
        playButton->setMinimumHeight(28);
        skipForward->setMinimumHeight(28);
        row->addWidget(skipBack);
        row->addWidget(playButton, 2);
        row->addWidget(skipForward);
        layout->addWidget(skip);
        layout->addLayout(row);
        connect(skipBack, &QPushButton::clicked, this, [this]() { emit skipPressed(-1); });
        connect(skipForward, &QPushButton::clicked, this, [this]() { emit skipPressed(1); });
        connect(playButton, &QPushButton::clicked, this, &RemoteWidget::playPressed);
        needsSource << skipBack << playButton << skipForward;
    }

    // The jog mode, the display, the side of the disc and the pause
    {
        QHBoxLayout *row = new QHBoxLayout();
        row->setSpacing(4);
        jogModeButton = key(tr("JOG\nMODE"), tr("The wheel jogs (a picture a step) or is a shuttle ring (its angle is the speed)"), "fn");
        displayButton = key(tr("DISPLAY"), tr("Show the numbers of the disc over the picture"), "fn");
        sideA = key("A", tr("The side of the disc that the VBI gives (the capture has one side)"), "side");
        sideB = key("B", tr("The side of the disc that the VBI gives (the capture has one side)"), "side");
        sideA->setEnabled(false);
        sideB->setEnabled(false);
        pauseButton = key(tr("PAUSE"), tr("Stop on this picture"), "fn");
        QLabel *side = new QLabel(tr("DISC SIDE"), this);
        QVBoxLayout *sides = new QVBoxLayout();
        sides->setSpacing(2);
        sides->addWidget(side, 0, Qt::AlignCenter);
        QHBoxLayout *sideRow = new QHBoxLayout();
        sideRow->setSpacing(3);
        sideRow->addWidget(sideA);
        sideRow->addWidget(sideB);
        sides->addLayout(sideRow);
        row->addWidget(jogModeButton);
        row->addWidget(displayButton);
        row->addLayout(sides);
        row->addWidget(pauseButton);
        layout->addLayout(row);
        connect(jogModeButton, &QPushButton::clicked, this, [this]() {
            const bool jog = !wheel->jogMode();
            setJogMode(jog);
            emit jogModeChanged(jog);
        });
        connect(displayButton, &QPushButton::clicked, this, &RemoteWidget::displayPressed);
        connect(pauseButton, &QPushButton::clicked, this, &RemoteWidget::pausePressed);
        needsSource << pauseButton;
    }

    // The wheel
    wheel = new JogWheel(this);
    layout->addWidget(wheel, 1);
    connect(wheel, &JogWheel::jogged, this, &RemoteWidget::jogged);
    connect(wheel, &JogWheel::shuttled, this, &RemoteWidget::shuttled);
    setJogMode(true);
    setDisplay(QStringLiteral("CHAPTER"), QStringLiteral("_"), QString());
    setSourceLoaded(false);
}

void RemoteWidget::setDisplay(const QString &mode, const QString &entry, const QString &state)
{
    modeButton->setText(mode);
    entryLabel->setText(entry);
    stateLabel->setText(state);
}

void RemoteWidget::setJogMode(bool jog)
{
    wheel->setJogMode(jog);
    setLit(jogModeButton, jog);
}

void RemoteWidget::setDisplayOn(bool on)
{
    setLit(displayButton, on);
}

void RemoteWidget::setMemoryStored(bool stored)
{
    setLit(memoryButton, stored);
}

void RemoteWidget::setStillWithSound(bool on)
{
    setLit(stillButton, on);
}

void RemoteWidget::setRepeatPoints(bool aSet, bool bSet)
{
    setLit(repeatA, aSet);
    setLit(repeatB, bSet);
}

void RemoteWidget::setAudioChoice(bool digitalAvailable, bool analogueAvailable, bool digital)
{
    digitalAnalogButton->setEnabled(digitalAvailable || analogueAvailable);
    audioLabel->setText(digitalAvailable || analogueAvailable ? (digital ? tr("DIG") : tr("ANA")) : QString());
    setLit(digitalAnalogButton, digital);
}

void RemoteWidget::setTransport(const QString &state)
{
    setLit(playButton, state == QLatin1String("play"));
    setLit(pauseButton, state == QLatin1String("pause"));
}

void RemoteWidget::setSide(qint32 side)
{
    setLit(sideA, side == 0);
    setLit(sideB, side == 1);
}

void RemoteWidget::setCx(bool on)
{
    setLit(cxButton, on);
}

void RemoteWidget::setSourceLoaded(bool loaded)
{
    for (QPushButton *button: needsSource) button->setEnabled(loaded);
    wheel->setEnabled(loaded);
    if (!loaded) {
        setTransport(QString());
        setSide(-1);
        setCx(false);
        setRepeatPoints(false, false);
        setMemoryStored(false);
        setStillWithSound(false);
        digitalAnalogButton->setEnabled(false);
    }
}

void RemoteWidget::setSearchAvailable(bool chapters, bool pictures, bool times)
{
    modeButton->setToolTip(tr("What the number is for: a chapter (%1), a picture (%2), a time (%3) or a frame (a press changes it)")
                           .arg(chapters ? tr("the disc has them") : tr("none on this disc"))
                           .arg(pictures ? tr("the disc has them") : tr("none on this disc"))
                           .arg(times ? tr("the disc has them") : tr("none on this disc")));
}

// The digits of the keyboard are the keys of the remote when it has the keyboard
void RemoteWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        QWidget::keyPressEvent(event);
        return;
    }
    const qint32 code = event->key();
    if (code >= Qt::Key_0 && code <= Qt::Key_9) {
        emit digitPressed(code - Qt::Key_0);
    } else if (code == Qt::Key_Return || code == Qt::Key_Enter) {
        emit searchPressed();
    } else if (code == Qt::Key_Backspace || code == Qt::Key_Delete) {
        emit clearPressed();
    } else if (code == Qt::Key_Plus) {
        emit plusTenPressed();
    } else {
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void RemoteWidget::setRaw(bool raw)
{
    setLit(chromaButton, raw);
}

void RemoteWidget::setMuted(bool muted)
{
    setLit(muteButton, muted);
}

void RemoteWidget::setViewMode(const QString &name)
{
    viewButton->setText(name);
}

void RemoteWidget::setFilter(bool on, bool available)
{
    setLit(filterButton, on);
    filterButton->setEnabled(available);
    if (!available) filterButton->setToolTip(tr("This build has no ffmpeg filters"));
}
