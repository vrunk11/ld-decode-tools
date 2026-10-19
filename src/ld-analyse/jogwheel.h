/******************************************************************************
 * jogwheel.h
 * ld-analyse - the jog and shuttle wheel of the remote control
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef JOGWHEEL_H
#define JOGWHEEL_H

#include <QWidget>

class QVariantAnimation;

// A wheel that is turned with the mouse (or its wheel). In the jog mode every step of its turn is a picture forward or backward;
// in the shuttle mode its angle from the centre is the speed, and it goes back to the centre (and the picture stops) when it is let go.
class JogWheel : public QWidget
{
    Q_OBJECT
public:
    explicit JogWheel(QWidget *parent = nullptr);

    void setJogMode(bool jog);
    bool jogMode() const { return jog; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // Degrees of turn that make one step in the jog mode, and the travel of the ring in the shuttle mode (each side of the centre)
    static constexpr double DEGREES_PER_STEP = 12.0;
    static constexpr double SHUTTLE_TRAVEL = 110.0;

signals:
    void jogged(qint32 steps);           // positive: forward
    void shuttled(double deflection);    // -1 to 1 (the part of the travel), 0 when let go

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    bool jog = true;
    bool dragging = false;
    double lastAngle = 0.0;       // the angle of the mouse round the centre, as the last event saw it
    double pending = 0.0;         // the turn that is not yet a whole step
    double shown = 0.0;           // the turn that is drawn (degrees, clockwise)
    double travelled = 0.0;       // the shuttle: from the centre
    QVariantAnimation *springBack;
    double angleAt(const QPointF &point) const;
    QPointF centre() const;
    double radius() const;
};

#endif // JOGWHEEL_H
