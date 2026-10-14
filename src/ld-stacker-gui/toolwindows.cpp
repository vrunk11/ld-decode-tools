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
#include <QPixmap>
#include <QVBoxLayout>

SnrWindow::SnrWindow(QWidget *parent)
    : QDialog(parent)
{
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    setWindowTitle(tr("SNR"));
    resize(640, 260);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    plot = new SnrPlot;
    plot->setMinimumHeight(160);
    layout->addWidget(plot, 1);
    connect(plot, &SnrPlot::frameRequested, this, &SnrWindow::frameRequested);
}

void SnrWindow::setContent(const QString &title, const QVector<float> &values, const QColor &color,
                           const QMap<qint32, float> &stack, const QColor &stackColor, qint32 marker)
{
    setWindowTitle(title);
    plot->setSeries(values, color, tr("White SNR (metadata); the stack, as measured so far, behind"));
    plot->setReference(stack, stackColor);
    plot->setMarker(marker);
}

void SnrWindow::setMarker(qint32 frame)
{
    plot->setMarker(frame);
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

void StackOutputWindow::setSmooth(bool newSmooth)
{
    smooth = newSmooth;
    showScaled();
}

void StackOutputWindow::showScaled()
{
    if (image.isNull()) return;
    label->setText(QString());
    label->setPixmap(QPixmap::fromImage(image).scaled(label->size(), Qt::KeepAspectRatio,
                                                      smooth ? Qt::SmoothTransformation : Qt::FastTransformation));
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
