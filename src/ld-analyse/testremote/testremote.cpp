/******************************************************************************
 * testremote.cpp
 * ld-analyse - unit test for the remote control: the number typed, the speeds, the keys
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QApplication>
#include <QKeyEvent>
#include <QPushButton>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>
#include <cstdio>
#include <cstdlib>

#include "chaptermarks.h"
#include "chapterlistdialog.h"
#include "infopanel.h"
#include "jogwheel.h"
#include "pintitlebar.h"
#include "remotelogic.h"
#include "remotewidget.h"
#include "seekbar.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            std::fflush(stderr); \
            std::exit(1); \
        } \
    } while (0)

// What the signals of a widget gave (QSignalSpy is of Qt Test, which is not in the build): the first argument of each, or nothing
class Spy : public QObject
{
public:
    template<class Obj> Spy(Obj *sender, void (Obj::*signal)()) { connect(sender, signal, this, [this]() { values.append(QVariant()); }); }
    template<class Obj> Spy(Obj *sender, void (Obj::*signal)(qint32)) { connect(sender, signal, this, [this](qint32 v) { values.append(v); }); }
    template<class Obj> Spy(Obj *sender, void (Obj::*signal)(bool)) { connect(sender, signal, this, [this](bool v) { values.append(v); }); }
    template<class Obj> Spy(Obj *sender, void (Obj::*signal)(double)) { connect(sender, signal, this, [this](double v) { values.append(v); }); }
    qint32 size() const { return values.size(); }
    bool isEmpty() const { return values.isEmpty(); }
    void clear() { values.clear(); }
    QVariant first() const { return values.first(); }
    QVariant last() const { return values.last(); }
    QVector<QVariant> values;
    typename QVector<QVariant>::const_iterator begin() const { return values.cbegin(); }
    typename QVector<QVariant>::const_iterator end() const { return values.cend(); }
};

static void typeDigits(RemoteKeypad &keypad, const char *digits)
{
    for (const char *c = digits; *c; c++) keypad.digit(*c - '0');
}

static void testKeypad()
{
    RemoteKeypad keypad;
    CHECK(keypad.isEmpty() && keypad.value() == -1);

    // Seven digits at most whatever the mode (the frame search takes the digits as a frame number); the others are not taken
    keypad.setMode(SearchMode::Chapter);
    typeDigits(keypad, "123");
    CHECK(keypad.value() == 123 && keypad.text() == "123" && keypad.number() == 123);
    typeDigits(keypad, "456789");
    CHECK(keypad.value() == 1234567);
    keypad.clear();
    CHECK(keypad.isEmpty() && keypad.text() == "_");

    // +10 makes ten more (from nothing too), and a first zero is not kept
    keypad.plusTen();
    CHECK(keypad.value() == 10);
    keypad.plusTen();
    CHECK(keypad.value() == 20);
    keypad.clear();
    typeDigits(keypad, "05");
    CHECK(keypad.value() == 5);

    // A picture
    keypad.setMode(SearchMode::Picture);
    CHECK(keypad.isEmpty());
    typeDigits(keypad, "123456");
    CHECK(keypad.value() == 123456);

    // A time is hours, minutes and seconds, the last digits being seconds: 1230 is 12 minutes 30 seconds
    keypad.setMode(SearchMode::Time);
    typeDigits(keypad, "1230");
    CHECK(keypad.value() == 12 * 60 + 30);
    CHECK(keypad.text() == "0:12:30");
    keypad.clear();
    typeDigits(keypad, "10000");
    CHECK(keypad.value() == 3600);
    keypad.clear();
    typeDigits(keypad, "5");
    keypad.plusTen();
    CHECK(keypad.value() == 15);
    CHECK(keypad.text() == "0:00:15");
    keypad.clear();
    CHECK(keypad.text() == "-:--:--");
    // (the digits of a time are one number too, for a frame search)
    typeDigits(keypad, "1230");
    CHECK(keypad.number() == 1230);
    keypad.clear();
    CHECK(keypad.number() == -1);

    // Changing the way of searching begins the number again
    typeDigits(keypad, "12");
    keypad.setMode(SearchMode::Frame);
    CHECK(keypad.isEmpty());
    typeDigits(keypad, "1234567");
    CHECK(keypad.value() == 1234567);

    // The modes that follow one another: the chapter, the picture and the time (only the ones the disc has); the frame has a key of its own
    CHECK(RemoteKeypad::nextMode(SearchMode::Chapter, true, true, true) == SearchMode::Picture);
    CHECK(RemoteKeypad::nextMode(SearchMode::Chapter, true, false, true) == SearchMode::Time);
    CHECK(RemoteKeypad::nextMode(SearchMode::Time, true, true, true) == SearchMode::Chapter);
    CHECK(RemoteKeypad::nextMode(SearchMode::Time, false, true, false) == SearchMode::Picture);
    CHECK(RemoteKeypad::nextMode(SearchMode::Chapter, false, false, false) == SearchMode::Chapter);
    CHECK(RemoteKeypad::nextMode(SearchMode::Frame, true, true, true) == SearchMode::Chapter);
}

static void testSpeeds()
{
    // The ladder goes up and down and stops at its ends
    CHECK(RemoteSpeed::step(1.0, 1) == 2.0);
    CHECK(RemoteSpeed::step(1.0, -1) == 0.5);
    CHECK(RemoteSpeed::step(32.0, 1) == 32.0);
    CHECK(RemoteSpeed::step(0.25, -1) == 0.25);
    CHECK(RemoteSpeed::step(-8.0, 1) == 16.0);

    // The shuttle ring: still near the centre, then the steps, the sign being the way, the highest at the end
    CHECK(RemoteSpeed::fromDeflection(0.0) == 0.0);
    CHECK(RemoteSpeed::fromDeflection(0.04) == 0.0);
    CHECK(RemoteSpeed::fromDeflection(0.1) == 0.25);
    CHECK(RemoteSpeed::fromDeflection(-0.1) == -0.25);
    CHECK(RemoteSpeed::fromDeflection(1.0) == 32.0);
    CHECK(RemoteSpeed::fromDeflection(-1.0) == -32.0);
    double last = 0.0;
    for (double d = 0.0; d <= 1.0; d += 0.01) {
        const double speed = RemoteSpeed::fromDeflection(d);
        CHECK(speed >= last);
        last = speed;
    }

    CHECK(RemoteSpeed::text(2.0) == "x2");
    CHECK(RemoteSpeed::text(-0.25) == "-x1/4");
    CHECK(clockText(3725) == "1:02:05");
}

static void testRemote()
{
    RemoteWidget remote;
    remote.show();
    remote.setSourceLoaded(true);

    // The keys are signals
    Spy digits(&remote, &RemoteWidget::digitPressed);
    Spy search(&remote, &RemoteWidget::searchPressed);
    Spy clear(&remote, &RemoteWidget::clearPressed);
    Spy plusTen(&remote, &RemoteWidget::plusTenPressed);
    for (QPushButton *button: remote.findChildren<QPushButton *>()) {
        if (button->text() == "7") button->click();
        if (button->text() == "C") button->click();
        if (button->text() == "+10") button->click();
    }
    CHECK(digits.size() == 1 && digits.first().toInt() == 7);
    CHECK(clear.size() == 1 && plusTen.size() == 1);

    // The keyboard does the same when the remote has it
    QKeyEvent five(QEvent::KeyPress, Qt::Key_5, Qt::NoModifier, "5");
    QApplication::sendEvent(&remote, &five);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(&remote, &enter);
    CHECK(digits.size() == 2 && digits.last().toInt() == 5);
    CHECK(search.size() == 1);
    // (a key with Ctrl is not a digit of the remote: it is left to the shortcuts)
    QKeyEvent ctrlFive(QEvent::KeyPress, Qt::Key_5, Qt::ControlModifier, "5");
    QApplication::sendEvent(&remote, &ctrlFive);
    CHECK(digits.size() == 2);

    // Without a capture the keys that act on it are off
    remote.setSourceLoaded(false);
    for (QPushButton *button: remote.findChildren<QPushButton *>()) {
        if (button->text() == "7") CHECK(!button->isEnabled());
    }

    // The display shows what it is given
    remote.setDisplay("TIME", "0:12:30", "PLAY");
    bool found = false;
    for (QPushButton *button: remote.findChildren<QPushButton *>()) found = found || button->text() == "TIME";
    CHECK(found);
}

static void testJogWheel()
{
    JogWheel wheel;
    wheel.resize(170, 150);
    wheel.show();
    Spy jogged(&wheel, &JogWheel::jogged);
    Spy shuttled(&wheel, &JogWheel::shuttled);

    // The wheel of the mouse: a notch is a step
    QWheelEvent notch(QPointF(80, 80), QPointF(80, 80), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&wheel, &notch);
    CHECK(jogged.size() == 1 && jogged.first().toInt() == 1);
    QWheelEvent back(QPointF(80, 80), QPointF(80, 80), QPoint(), QPoint(0, -240), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&wheel, &back);
    CHECK(jogged.size() == 2 && jogged.last().toInt() == -2);

    // A drag from the top to the right goes round clockwise: forward steps, as many as the degrees say
    const QPointF centre(wheel.width() / 2.0, wheel.height() / 2.0 + 4);
    const QPointF top(centre.x(), centre.y() - 50);
    const QPointF right(centre.x() + 50, centre.y());
    jogged.clear();
    QMouseEvent press(QEvent::MouseButtonPress, top, wheel.mapToGlobal(top), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&wheel, &press);
    QMouseEvent move(QEvent::MouseMove, right, wheel.mapToGlobal(right), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&wheel, &move);
    qint32 steps = 0;
    for (const auto &call: jogged) steps += call.toInt();
    CHECK(steps == static_cast<qint32>(90.0 / JogWheel::DEGREES_PER_STEP));
    QMouseEvent release(QEvent::MouseButtonRelease, right, wheel.mapToGlobal(right), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&wheel, &release);
    CHECK(shuttled.isEmpty());

    // The shuttle ring: the angle is the speed, and it is 0 when the ring is let go
    wheel.setJogMode(false);
    shuttled.clear();
    QApplication::sendEvent(&wheel, &press);
    QApplication::sendEvent(&wheel, &move);
    CHECK(!shuttled.isEmpty());
    const double deflection = shuttled.last().toDouble();
    CHECK(deflection > 0.5 && deflection <= 1.0);
    QApplication::sendEvent(&wheel, &release);
    CHECK(shuttled.last().toDouble() == 0.0);
}

static void testInfoPanel()
{
    InfoPanel panel;
    panel.setRows("0:12:30", { { "Chapter", "3" }, { "Picture", "12345" } });
    CHECK(panel.headline() == "0:12:30");
    CHECK(panel.rows().size() == 2 && panel.rows()[1].second == "12345");
}

static void testSeekBar()
{
    SeekBar bar;
    bar.setRange(1, 101);
    bar.resize(300, 24);
    bar.show();
    Spy scrubbed(&bar, &SeekBar::scrubbed);
    Spy finished(&bar, &SeekBar::scrubFinished);

    // What the owner sets is not said back
    bar.setValue(50);
    CHECK(scrubbed.isEmpty());

    // A click goes to the place that was clicked, the middle of the bar being the middle of the range
    const QPoint middle(bar.width() / 2, 12);
    QMouseEvent press(QEvent::MouseButtonPress, middle, bar.mapToGlobal(middle), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &press);
    CHECK(bar.isScrubbing());
    CHECK(bar.value() >= 45 && bar.value() <= 57);

    // A drag follows the mouse, to the ends and not beyond
    const QPoint far(bar.width() + 40, 12);
    QMouseEvent toEnd(QEvent::MouseMove, far, bar.mapToGlobal(far), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &toEnd);
    CHECK(bar.value() == 101);
    const QPoint before(-40, 12);
    QMouseEvent toStart(QEvent::MouseMove, before, bar.mapToGlobal(before), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &toStart);
    CHECK(bar.value() == 1);
    CHECK(scrubbed.size() >= 3 && scrubbed.last().toInt() == 1);

    // Letting go says so, once
    QMouseEvent release(QEvent::MouseButtonRelease, before, bar.mapToGlobal(before), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &release);
    CHECK(!bar.isScrubbing() && finished.size() == 1);
}

static void testChapterMarks()
{
    QSlider slider(Qt::Horizontal);
    slider.setRange(1, 101);
    slider.resize(300, 24);
    slider.show();
    ChapterMarks marks(&slider);

    // The layer takes the size of the slider, and the marks are put by the range of the slider: the start, the middle and the end of it
    CHECK(marks.size() == slider.size());
    marks.setMarks({ 1, 51, 101, 500, 0 });
    const QVector<qint32> positions = marks.markPositions();
    CHECK(positions.size() == 3);
    CHECK(positions[0] < positions[1] && positions[1] < positions[2]);
    CHECK(qAbs(positions[1] - slider.width() / 2) <= 4);
    CHECK(positions[0] <= 16 && positions[2] >= slider.width() - 16);

    // It follows the slider when that is resized, and the mouse goes through it
    slider.resize(500, 24);
    QApplication::processEvents();
    CHECK(marks.size() == slider.size());
    CHECK(marks.testAttribute(Qt::WA_TransparentForMouseEvents));
    CHECK(marks.markPositions()[1] > positions[1]);

    // With no range there is nothing to draw
    slider.setRange(0, 0);
    CHECK(marks.markPositions().isEmpty());
}

static void testChapterList()
{
    ChapterListDialog dialog;
    dialog.show();
    Spy goTo(&dialog, &ChapterListDialog::goToFrame);

    // No chapters: a message, and nothing to go to
    CHECK(dialog.rowCount() == 0);
    dialog.setChapters({}, 1);
    CHECK(dialog.currentRow() == -1);

    // The chapters, and the one the frame is in is the one that is chosen
    ChapterListDialog::Row first, second, third;
    first.chapter = 1; first.frame = 1; first.discSeconds = 0;
    second.chapter = 2; second.frame = 750; second.discSeconds = 30; second.captureSeconds = 30;
    third.chapter = 3; third.frame = 1500; third.picture = 1500; third.captureSeconds = 60;
    dialog.setChapters({ first, second, third }, 800);
    CHECK(dialog.rowCount() == 3);
    CHECK(dialog.currentRow() == 1);

    // The button goes to the frame where the chosen chapter begins
    bool pressed = false;
    for (QPushButton *button: dialog.findChildren<QPushButton *>()) {
        if (button->text() == "Go to") {
            button->click();
            pressed = true;
        }
    }
    CHECK(pressed);
    CHECK(goTo.size() == 1 && goTo.last().toInt() == 750);

    // Before the first chapter began, the first is chosen
    dialog.setChapters({ second, third }, 10);
    CHECK(dialog.currentRow() == 0);
}

static void testTitleBar()
{
    QWidget host;
    QVBoxLayout layout(&host);
    layout.setContentsMargins(0, 0, 0, 0);
    PinTitleBar bar("Title", &host);
    layout.addWidget(&bar);
    layout.addStretch(1);
    host.resize(220, 80);
    host.show();
    Spy pinned(&bar, &PinTitleBar::pinToggled);
    Spy closed(&bar, &PinTitleBar::closeRequested);

    // The pin is a toggle, and says when it is set by the owner too
    CHECK(!bar.pinned());
    bar.setPinned(true);
    CHECK(bar.pinned() && pinned.size() == 1 && pinned.last().toBool());
    bar.setPinned(false);
    CHECK(!bar.pinned() && pinned.size() == 2 && !pinned.last().toBool());

    // The close button asks for the window to be closed
    const QList<QToolButton *> buttons = bar.findChildren<QToolButton *>();
    CHECK(buttons.size() == 1);
    buttons.first()->click();
    CHECK(closed.size() == 1);

    // Dragging the bar moves the window by as much as the mouse moved
    const QPoint before = host.pos();
    const QPoint start(60, 10);
    QMouseEvent press(QEvent::MouseButtonPress, start, host.mapToGlobal(start), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &press);
    const QPoint to(start.x() + 30, start.y() + 20);
    QMouseEvent move(QEvent::MouseMove, to, host.mapToGlobal(start) + QPoint(30, 20), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &move);
    CHECK(host.pos() == before + QPoint(30, 20));
    QMouseEvent release(QEvent::MouseButtonRelease, to, host.mapToGlobal(start) + QPoint(30, 20), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &release);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    testKeypad();
    testSpeeds();
    testRemote();
    testJogWheel();
    testInfoPanel();
    testSeekBar();
    testTitleBar();
    testChapterMarks();
    testChapterList();
    std::printf("testremote: all checks passed\n");
    return 0;
}
