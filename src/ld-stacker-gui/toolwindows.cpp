/******************************************************************************
 * toolwindows.cpp
 * ld-stacker-gui - Disc stacking GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "toolwindows.h"

#include <QCloseEvent>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QVBoxLayout>

SnrWindow::SnrWindow(QWidget *parent)
    : QDialog(parent)
{
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    setWindowTitle(tr("SNR"));
    resize(640, 260);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    accentBar = new QFrame;
    accentBar->setFixedHeight(8);
    layout->addWidget(accentBar);
    graph = new SnrGraph(true);
    graph->setMinimumHeight(180);
    layout->addWidget(graph, 1);
    connect(graph, &SnrGraph::frameRequested, this, &SnrWindow::frameRequested);
    connect(graph, &SnrGraph::measureToggled, this, &SnrWindow::measureToggled);
}

void SnrWindow::setAccent(const QColor &color)
{
    accentBar->setStyleSheet(QString("background-color: %1;").arg(color.name()));
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QRectF(1, 1, 14, 14));
    painter.end();
    setWindowIcon(QIcon(pixmap));}

void SnrWindow::setContent(const QString &title, const QVector<float> &white, const QVector<float> &black,
                           const QVector<float> &stackWhite, const QVector<float> &stackBlack, qint32 marker)
{
    setWindowTitle(title);
    graph->setData(QString(), white, black, stackWhite, stackBlack);
    graph->setMarker(marker);
}

void SnrWindow::setMarker(qint32 frame)
{
    graph->setMarker(frame);
}

void SnrWindow::setBridgeGaps(bool bridge)
{
    graph->setBridgeGaps(bridge);
}

void SnrWindow::setMeasure(bool on, bool enabled)
{
    graph->setMeasure(on, enabled);
}

void SnrWindow::setRange(bool fixed, double low, double high)
{
    graph->setRange(fixed, low, high);
}

StackOutputWindow::StackOutputWindow(QWidget *parent)
    : QDialog(parent)
{
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    setWindowTitle(tr("Stack output"));
    resize(560, 440);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    label = new QLabel;
    label->setAlignment(Qt::AlignCenter);
    label->setMinimumSize(120, 90);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    layout->addWidget(label, 1);
    caption = new QLabel;
    caption->setAlignment(Qt::AlignCenter);
    layout->addWidget(caption);
}

void StackOutputWindow::setPicture(const QImage &newImage, const QString &text)
{
    image = newImage;
    caption->setText(text);
    showScaled();
}

void StackOutputWindow::setMessage(const QString &message)
{
    image = QImage();
    label->setPixmap(QPixmap());
    label->setText(message);
    caption->clear();
}

void StackOutputWindow::showScaled()
{
    if (image.isNull()) return;
    label->setText(QString());
    label->setPixmap(QPixmap::fromImage(image).scaled(label->size(), Qt::KeepAspectRatio,
                                                      Qt::FastTransformation));
}

void StackOutputWindow::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    showScaled();
}

void StackOutputWindow::closeEvent(QCloseEvent *event)
{
    QDialog::closeEvent(event);
    emit closed();
}

// ---------------------------------------------------------------------------------------------

DropoutWindow::DropoutWindow(QWidget *parent)
    : QDialog(parent)
{
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    setWindowTitle(tr("Dropouts"));
    resize(640, 220);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    accentBar = new QFrame;
    accentBar->setFixedHeight(8);
    layout->addWidget(accentBar);
    graph = new DropoutGraph;
    graph->setMinimumHeight(130);
    layout->addWidget(graph, 1);
    connect(graph, &DropoutGraph::frameRequested, this, &DropoutWindow::frameRequested);
}

void DropoutWindow::setAccent(const QColor &color)
{
    accentBar->setStyleSheet(QString("background-color: %1;").arg(color.name()));
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QRectF(1, 1, 14, 14));
    painter.end();
    setWindowIcon(QIcon(pixmap));
}

void DropoutWindow::setContent(const QString &title, const QVector<SnrCurve> &curves, qint32 marker)
{
    setWindowTitle(title);
    graph->setData(tr("Total length of the dropouts of each frame, in samples"), curves);
    graph->setMarker(marker);
}

void DropoutWindow::setMarker(qint32 frame)
{
    graph->setMarker(frame);
}

void DropoutWindow::setRange(bool fixed, double low, double high)
{
    graph->setRange(fixed, low, high);
}
