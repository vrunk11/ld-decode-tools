/******************************************************************************
 * themeeditordialog.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef THEMEEDITORDIALOG_H
#define THEMEEDITORDIALOG_H

#include <QDialog>
#include <QPalette>
#include <QVector>

class QColorDialog;
class QComboBox;
class QPushButton;

// View > Theme > Customize...: edits the colours of the custom theme. Every
// change, including dragging in the colour picker, is sent with
// previewChanged() so the caller can apply it to the whole application at
// once. Accepting keeps the palette (see palette()); rejecting means the
// caller should restore the previous theme.
class ThemeEditorDialog : public QDialog
{
    Q_OBJECT

public:
    // startPalette: the colours to begin with, usually the current ones
    explicit ThemeEditorDialog(const QPalette &startPalette, QWidget *parent = nullptr);

    // The edited palette, serialised as stored in the configuration
    QString palette() const;

signals:
    void previewChanged(const QString &serialisedPalette);

private:
    QPalette editedPalette;
    QPalette resetPalette;       // what Reset returns to
    QComboBox *startFromCombo;
    QVector<QPushButton *> swatches;
    QColorDialog *colourDialog = nullptr;
    int editedRole = -1;         // index in themeColourRoles() being picked
    QColor colourBeforePick;     // restored if the picker is cancelled

    void loadPalette(const QPalette &palette);
    void setRoleColour(int index, const QColor &colour);
    void updateSwatch(int index);
    void pickColour(int index);
    void emitPreview();
};

#endif // THEMEEDITORDIALOG_H
