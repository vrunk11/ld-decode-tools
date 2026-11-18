/******************************************************************************
 * vectorscopedialog.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2022 Adam Sampson
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef VECTORSCOPEDIALOG_H
#define VECTORSCOPEDIALOG_H

#include <QAbstractButton>
#include <QGraphicsPixmapItem>
#include <QDialog>

#include "componentframe.h"
#include "lddecodemetadata.h"
#include "tbcsource.h"

namespace Ui {
class VectorscopeDialog;
}

class VectorscopeDialog : public QDialog
{
    Q_OBJECT

public:
    explicit VectorscopeDialog(QWidget *parent = nullptr);
    ~VectorscopeDialog();

    // What the controls say (see prepare): the scope is drawn from it, which can be done away from the window
    struct Options {
        bool blend = false, defocus = false, all = true, first = false, second = false, graticuleNone = false, graticule75 = false;
    };
    // Set the controls as the view lets them be, and read them
    Options prepare(const TbcSource::ViewMode &viewMode, const bool isFirstField);
    // Draw the trace (it does not touch the window: any thread can)
    static QImage renderTrace(const ComponentFrame &componentFrame, const LdDecodeMetaData::VideoParameters &videoParameters, const Options &options);
    // Show a trace that was drawn
    void showTraceImage(const QImage &traceImage);

    void showTraceImage(const ComponentFrame &componentFrame, const LdDecodeMetaData::VideoParameters &videoParameters,
                        const TbcSource::ViewMode& viewMode, const bool isFirstField);

signals:
    void scopeChanged();

private slots:
    void on_defocusCheckBox_clicked();
    void on_blendColorCheckBox_clicked();
    void on_graticuleButtonGroup_buttonClicked(QAbstractButton *button);
    void on_fieldSelectButtonGroup_buttonClicked(QAbstractButton *button);

private:
    Ui::VectorscopeDialog *ui;

};

#endif // VECTORSCOPEDIALOG_H
