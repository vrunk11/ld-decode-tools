/******************************************************************************
 * themeeditordialog.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "themeeditordialog.h"
#include "theme.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

ThemeEditorDialog::ThemeEditorDialog(const QPalette &startPalette, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Customize theme"));

    QVBoxLayout *layout = new QVBoxLayout(this);

    // Start from one of the presets, or from the colours in use
    QHBoxLayout *startLayout = new QHBoxLayout;
    startLayout->addWidget(new QLabel(tr("Start from:"), this));
    startFromCombo = new QComboBox(this);
    startFromCombo->addItem(tr("Current colours"), -1);
    startFromCombo->addItem(tr("Soft light"), static_cast<int>(ThemeMode::SoftLight));
    startFromCombo->addItem(tr("Dim"), static_cast<int>(ThemeMode::Dim));
    startFromCombo->addItem(tr("Dark grey"), static_cast<int>(ThemeMode::DarkGrey));
    startLayout->addWidget(startFromCombo, 1);
    layout->addLayout(startLayout);

    // One colour swatch per editable colour
    QFormLayout *form = new QFormLayout;
    const QVector<ThemeColourRole> &roles = themeColourRoles();
    for (int index = 0; index < roles.size(); index++) {
        QPushButton *swatch = new QPushButton(this);
        swatch->setMinimumWidth(120);
        swatch->setToolTip(tr("Click to choose the colour"));
        connect(swatch, &QPushButton::clicked, this, [this, index]() { pickColour(index); });
        swatches.append(swatch);
        form->addRow(tr(roles[index].label) + ":", swatch);
    }
    layout->addLayout(form);

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel
                                                     | QDialogButtonBox::Reset, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, this, [this]() {
        loadPalette(resetPalette);
        emitPreview();
    });

    // Choosing a starting point replaces all the colours, and becomes what Reset returns to
    const QPalette currentPalette = startPalette;
    connect(startFromCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, currentPalette](int item) {
        const int mode = startFromCombo->itemData(item).toInt();
        resetPalette = (mode < 0) ? currentPalette : themePalette(static_cast<ThemeMode>(mode));
        loadPalette(resetPalette);
        emitPreview();
    });

    resetPalette = startPalette;
    loadPalette(startPalette);
}

QString ThemeEditorDialog::palette() const
{
    return serializePalette(editedPalette);
}

void ThemeEditorDialog::loadPalette(const QPalette &palette)
{
    editedPalette = deserializePalette(serializePalette(palette));
    for (int index = 0; index < swatches.size(); index++) updateSwatch(index);
}

void ThemeEditorDialog::setRoleColour(int index, const QColor &colour)
{
    // Rebuild through the serialised form so that linked roles (disabled
    // text) follow, exactly as when the palette is loaded from the settings
    QString serialised = serializePalette(editedPalette);
    const QString key = QLatin1String(themeColourRoles()[index].key);
    QStringList entries = serialised.split(';');
    for (QString &entry : entries) {
        if (entry.section('=', 0, 0) == key) entry = key + "=" + colour.name();
    }
    editedPalette = deserializePalette(entries.join(';'));
    updateSwatch(index);
}

void ThemeEditorDialog::updateSwatch(int index)
{
    const ThemeColourRole &entry = themeColourRoles()[index];
    const QColor colour = editedPalette.color(entry.group == QPalette::All ? QPalette::Active : entry.group, entry.role);
    const QString textColour = colour.lightness() < 128 ? "#ffffff" : "#000000";
    swatches[index]->setText(colour.name());
    swatches[index]->setStyleSheet(QString("QPushButton { background-color: %1; color: %2; border: 1px solid #808080; padding: 3px; }")
                                       .arg(colour.name(), textColour));
}

// Open the colour picker for one colour. It is not modal, and every movement
// in it is previewed straight away; cancelling it restores the colour.
void ThemeEditorDialog::pickColour(int index)
{
    if (!colourDialog) {
        colourDialog = new QColorDialog(this);
        colourDialog->setOption(QColorDialog::NoButtons, false);
        connect(colourDialog, &QColorDialog::currentColorChanged, this, [this](const QColor &colour) {
            if (editedRole < 0 || !colour.isValid()) return;
            setRoleColour(editedRole, colour);
            emitPreview();
        });
        connect(colourDialog, &QColorDialog::rejected, this, [this]() {
            if (editedRole < 0) return;
            setRoleColour(editedRole, colourBeforePick);
            emitPreview();
            editedRole = -1;
        });
        connect(colourDialog, &QColorDialog::accepted, this, [this]() { editedRole = -1; });
    }

    const ThemeColourRole &entry = themeColourRoles()[index];
    colourBeforePick = editedPalette.color(entry.group == QPalette::All ? QPalette::Active : entry.group, entry.role);

    // Set the starting colour before choosing the role, so it is not applied twice
    editedRole = -1;
    colourDialog->setCurrentColor(colourBeforePick);
    editedRole = index;

    colourDialog->setWindowTitle(tr("Colour: %1").arg(tr(entry.label)));
    colourDialog->show();
    colourDialog->raise();
    colourDialog->activateWindow();
}

void ThemeEditorDialog::emitPreview()
{
    emit previewChanged(serializePalette(editedPalette));
}
