/******************************************************************************
 * rendererpanel.cpp
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "rendererpanel.h"

#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QMessageBox>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QThread>

RendererPanel::RendererPanel(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    const auto makeHint = [this](const QString &text) {
        QLabel *hint = new QLabel(text, this);
        hint->setWordWrap(true);
        hint->setForegroundRole(QPalette::PlaceholderText);
        return hint;
    };

    // ---- VLC ----
    vlcPage = new QWidget(this);
    QVBoxLayout *vlcLayout = new QVBoxLayout(vlcPage);
    vlcLayout->setContentsMargins(0, 0, 0, 0);

    QHBoxLayout *pathRow = new QHBoxLayout;
    pathRow->addWidget(new QLabel(tr("VLC:"), vlcPage));
    pathEdit = new QLineEdit(vlcPage);
    pathEdit->setPlaceholderText(tr("Found by itself (type the path of vlc if it is not)"));
    pathRow->addWidget(pathEdit, 1);
    QPushButton *browse = new QPushButton(tr("Browse..."), vlcPage);
    pathRow->addWidget(browse);
    vlcLayout->addLayout(pathRow);
    statusLabel = makeHint(QString());
    vlcLayout->addWidget(statusLabel);

    // What VLC has is asked of it the first time (and kept): it is asked again only here, for when a module was added to VLC or the list is not what it should be
    reloadButton = new QPushButton(tr("Ask VLC again"), vlcPage);
    reloadButton->setToolTip(tr("VLC is asked for its video outputs, deinterlacers and options the first time, and what it said is kept. Ask again if a module was added to VLC."));
    vlcLayout->addWidget(reloadButton, 0, Qt::AlignLeft);

    QHBoxLayout *voutRow = new QHBoxLayout;
    voutRow->addWidget(new QLabel(tr("Video output:"), vlcPage));
    voutCombo = new QComboBox(vlcPage);
    voutCombo->setEditable(true);      // (the name of a module that is not in the list can be typed)
    voutCombo->addItem(tr("VLC's own choice"), QString());
    voutRow->addWidget(voutCombo, 1);
    vlcLayout->addLayout(voutRow);

    QHBoxLayout *deinterlaceRow = new QHBoxLayout;
    deinterlaceRow->addWidget(new QLabel(tr("Deinterlacing:"), vlcPage));
    deinterlaceCombo = new QComboBox(vlcPage);
    deinterlaceCombo->addItem(tr("VLC's default mode"), QString());
    deinterlaceRow->addWidget(deinterlaceCombo, 1);
    vlcLayout->addLayout(deinterlaceRow);
    vlcLayout->addWidget(makeHint(tr("Used in the Deinterlaced view only.")));

    // The options that VLC lists for the modules of the picture and for the video output, each with its own switch: an option that is not set is VLC's (its
    // preferences). A change of one starts VLC again with it.
    effectsWidget = new QWidget(vlcPage);
    effectsLayout = new QVBoxLayout(effectsWidget);
    effectsLayout->setContentsMargins(0, 0, 0, 0);
    QScrollArea *scroll = new QScrollArea(vlcPage);
    scroll->setWidget(effectsWidget);
    scroll->setWidgetResizable(true);
    scroll->setMinimumHeight(180);
    scroll->setMaximumHeight(300);
    vlcLayout->addWidget(scroll);

    QHBoxLayout *extraRow = new QHBoxLayout;
    extraRow->addWidget(new QLabel(tr("Other arguments:"), vlcPage));
    extraEdit = new QLineEdit(vlcPage);
    extraEdit->setPlaceholderText(tr("For example --video-filter=... or --aspect-ratio=..."));
    extraRow->addWidget(extraEdit, 1);
    vlcLayout->addLayout(extraRow);
    layout->addWidget(vlcPage);

    // ---- another program ----
    commandPage = new QWidget(this);
    QVBoxLayout *commandLayout = new QVBoxLayout(commandPage);
    commandLayout->setContentsMargins(0, 0, 0, 0);
    commandEdit = new QLineEdit(commandPage);
    commandEdit->setPlaceholderText(tr("The command line of the program that shows the picture"));
    commandLayout->addWidget(commandEdit);
    commandLayout->addWidget(makeHint(tr("The picture is sent to the program as raw 8-bit RGB video on its standard input. {width}, {height} and {fps} stand for the size of "
                                         "the picture and the rate of the video, {aspect} for its shape (as 4:3 does) and {winid} for the window of ld-analyse that the program "
                                         "is to draw in: with it the picture is shown in the window, without it the program has its own.")));
    layout->addWidget(commandPage);

    previewLabel = makeHint(QString());
    previewLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(previewLabel);

    connect(reloadButton, &QPushButton::clicked, this, [this]() {
        if (loading) return;
        // What VLC has is asked again, and its preferences are read again: the video output, the deinterlacing and the options are set as they are in VLC (when
        // the answer comes, see helpLoaded). The path of VLC and the other arguments are kept.
        const auto answer = QMessageBox::question(this, tr("Ask VLC again"), tr("VLC will be asked again for what it has, and the video output, the deinterlacing and the options will be set as they are in the preferences of VLC. Go on?"));
        if (answer != QMessageBox::Yes) return;
        resetFromVlc = true;
        helpReady = false;
        loadHelp(true);
    });
    connect(browse, &QPushButton::clicked, this, &RendererPanel::chooseVlc);
    connect(pathEdit, &QLineEdit::editingFinished, this, [this]() {
        loadHelp();
        emit changed();
    });
    connect(voutCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        if (building) return;
        rebuildEffects();
        updatePreview();
        emit changed();
    });
    // (a name that was typed in the list of the video outputs)
    connect(voutCombo->lineEdit(), &QLineEdit::editingFinished, this, [this]() {
        if (building) return;
        rebuildEffects();
        updatePreview();
        emit changed();
    });
    const auto edited = [this]() {
        if (building) return;
        updatePreview();
        emit changed();
    };
    connect(deinterlaceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        if (building) return;
        rebuildEffects();
        updatePreview();
        emit changed();
    });
    connect(extraEdit, &QLineEdit::textChanged, this, edited);
    connect(commandEdit, &QLineEdit::textChanged, this, edited);

    vlcPage->hide();
    commandPage->hide();
}

RendererPanel::~RendererPanel() = default;

void RendererPanel::setEngine(int newEngine)
{
    // (ffmpeg is not this panel's: nothing is asked of VLC until the panel is for it)
    engine = newEngine;
    vlcPage->setVisible(engine == RendererSettings::Vlc);
    commandPage->setVisible(engine == RendererSettings::Command);
    updatePreview();
}

void RendererPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (engine == RendererSettings::Vlc && !helpReady && !loading) loadHelp();
}

void RendererPanel::setSettings(const RendererSettings &settings)
{
    building = true;
    current = settings;
    pathEdit->setText(settings.vlcPath);
    extraEdit->setText(settings.extra);
    commandEdit->setText(settings.command);
    // (the lists are filled when VLC has said what it has: what was chosen is put back then)
    const int vout = voutCombo->findData(settings.vout);
    if (vout >= 0) voutCombo->setCurrentIndex(vout);
    const int deinterlace = deinterlaceCombo->findData(settings.deinterlace);
    if (deinterlace >= 0) deinterlaceCombo->setCurrentIndex(deinterlace);
    building = false;
    setEngine(settings.engine);
    if (helpReady) rebuildEffects();
    updatePreview();
}

RendererSettings RendererPanel::settings() const
{
    RendererSettings result = current;
    result.engine = engine;
    result.vlcPath = pathEdit->text().trimmed();
    // (the lists are empty until VLC has said what it has: what was chosen stays)
    result.vout = helpReady ? voutValue() : current.vout;
    result.deinterlace = helpReady ? deinterlaceCombo->currentData().toString() : current.deinterlace;
    result.extra = extraEdit->text();
    result.command = commandEdit->text();
    // The options that are set; those of the lists that are not there yet are as they were
    QStringList options;
    if (rows.isEmpty()) {
        options = current.options;
    } else {
        for (const OptionRow &row: rows) {
            if (!row.set->isChecked()) continue;
            QString text;
            if (row.isBoolean) text = static_cast<QCheckBox *>(row.value)->isChecked() ? QStringLiteral("true") : QStringLiteral("false");
            else if (row.isChoice) text = static_cast<QComboBox *>(row.value)->currentData().toString();
            else if (row.isInteger) text = QString::number(static_cast<QSpinBox *>(row.value)->value());
            else text = QString::number(static_cast<QDoubleSpinBox *>(row.value)->value(), 'g', 6);
            options << QString("%1:%2=%3").arg(row.module, row.name, text);
        }
    }
    result.options = options;
    return result;
}

// The video output chosen: the module of the entry that is chosen, or what was typed
QString RendererPanel::voutValue() const
{
    const int index = voutCombo->currentIndex();
    if (index >= 0 && voutCombo->itemText(index) == voutCombo->currentText()) return voutCombo->itemData(index).toString();
    const QString typed = voutCombo->currentText().trimmed();
    return typed == tr("VLC's own choice") ? QString() : typed;
}

void RendererPanel::chooseVlc()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Where is VLC"), pathEdit->text());
    if (file.isEmpty()) return;
    pathEdit->setText(file);
    loadHelp();
    emit changed();
}

// What VLC has (its video outputs, its deinterlacers, the options of its modules) is asked on a thread of its own: it takes some seconds the first time
void RendererPanel::loadHelp(bool refresh)
{
    const QString path = VlcSupport::find(pathEdit->text().trimmed());
    if (path.isEmpty()) {
        helpReady = false;
        statusLabel->setText(tr("VLC was not found: type the path of it (or browse to it) above."));
        return;
    }
    if (helpReady && path == helpPath) {
        updateStatus();
        return;
    }
    loading = true;
    statusLabel->setText(tr("Asking %1 for its video outputs and options (a few seconds the first time)...").arg(path));
    QPointer<RendererPanel> self(this);
    QThread *thread = QThread::create([self, path, refresh]() {
        VlcSupport::Help loaded;
        QString error;
        const bool ok = VlcSupport::load(path, loaded, error, refresh);
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, path, ok, error, loaded]() {
            if (self) self->helpLoaded(path, ok, error, loaded);
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void RendererPanel::helpLoaded(const QString &path, bool ok, const QString &error, const VlcSupport::Help &loaded)
{
    loading = false;
    if (ok) {
        help = loaded;
    } else {
        // VLC could not be asked: the usual outputs and adjustments are offered, and the name of any module can be typed
        VlcSupport::fallback(help);
        failure = error;
    }
    helpReady = true;
    helpPath = path;

    building = true;
    const QString vout = current.vout.isEmpty() ? voutValue() : current.vout;
    const QString deinterlace = deinterlaceCombo->currentData().toString().isEmpty() ? current.deinterlace : deinterlaceCombo->currentData().toString();
    voutCombo->clear();
    voutCombo->addItem(tr("VLC's own choice"), QString());
    for (const QString &module: help.videoOutputs) {
        const QString name = help.moduleNames.value(module);
        voutCombo->addItem(name.isEmpty() ? module : QString("%1 (%2)").arg(name, module), module);
    }
    const int voutIndex = voutCombo->findData(vout);
    if (voutIndex >= 0) voutCombo->setCurrentIndex(voutIndex);
    else if (!vout.isEmpty()) voutCombo->setEditText(vout);       // (a module that is not in the list: it was typed)
    else voutCombo->setCurrentIndex(0);
    deinterlaceCombo->clear();
    deinterlaceCombo->addItem(tr("VLC's default mode"), QString());
    for (const QString &mode: help.deinterlaceModes) deinterlaceCombo->addItem(mode, mode);
    // (the deinterlacers that were added to VLC, as VLC lists them: they are not modes of VLC's, VLC tries them before its own)
    for (const QString &module: help.deinterlacers) {
        const QString name = help.moduleNames.value(module);
        deinterlaceCombo->addItem(name.isEmpty() ? module : QString("%1 (%2)").arg(name, module), "filter:" + module);
    }
    deinterlaceCombo->setCurrentIndex(qMax(0, deinterlaceCombo->findData(deinterlace)));
    building = false;

    rebuildEffects();
    updateStatus();
    updatePreview();

    // Asked again by the person: what is chosen is what is set in the preferences of VLC (VLC's own choice for what is not set there), and it is kept as what is chosen
    if (resetFromVlc) {
        resetFromVlc = false;
        building = true;
        const QString vout = help.vlcSettings.value("vout");
        voutCombo->setCurrentIndex(qMax(0, voutCombo->findData(help.videoOutputs.contains(vout) ? vout : QString())));
        const QString mode = help.vlcSettings.value("deinterlace-mode");
        deinterlaceCombo->setCurrentIndex(qMax(0, deinterlaceCombo->findData(help.deinterlaceModes.contains(mode) ? mode : QString())));
        current.options.clear();
        for (auto module = help.modules.constBegin(); module != help.modules.constEnd(); ++module) {
            for (const VlcSupport::Option &option: module.value()) {
                const auto set = help.vlcSettings.constFind(option.name);
                if (set == help.vlcSettings.constEnd()) continue;
                QString value = set.value();
                if (option.isBoolean) value = value.toInt() != 0 ? QStringLiteral("true") : QStringLiteral("false");
                current.options << QString("%1:%2=%3").arg(module.key(), option.name, value);
            }
        }
        rows.clear();
        building = false;
        rebuildEffects();
        updatePreview();
        emit changed();
    }
}

void RendererPanel::updateStatus()
{
    if (!helpReady) {
        statusLabel->clear();
    } else if (!failure.isEmpty()) {
        statusLabel->setText(tr("%1. The usual video outputs and adjustments are offered; the name of another video output (an upscaler that was added to VLC...) can be typed.").arg(failure));
    } else {
        statusLabel->setText(tr("VLC: %1 (%2 video outputs, %3 deinterlacers)").arg(helpPath).arg(help.videoOutputs.size()).arg(help.deinterlaceModes.size()));
    }
}

// The options of the modules of the picture, and of the video output that is chosen: a switch, a name and a control of the range VLC says, for each
void RendererPanel::rebuildEffects()
{
    // What is set now is kept across the rebuild (the video output or the deinterlacer change the list)
    QStringList keep = rows.isEmpty() ? current.options : settings().options;
    while (QLayoutItem *item = effectsLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    rows.clear();

    // The modules of the picture, then the video output and the deinterlacer that are chosen, when VLC lists options for them (the ones of a module that was added to
    // VLC, as an upscaler, are there as well). They are given to the libvlc of ld-analyse only: the preferences of VLC are not changed.
    QStringList modules = { "adjust", "sharpen", "gradfun", "grain" };
    const QString vout = voutValue();
    if (!vout.isEmpty() && help.modules.contains(vout)) modules.prepend(vout);
    const QString deinterlace = helpReady ? deinterlaceCombo->currentData().toString() : current.deinterlace;
    if (deinterlace.startsWith(QLatin1String("filter:"))) {
        const QString module = deinterlace.mid(7);
        if (help.modules.contains(module)) modules.insert(vout.isEmpty() || !help.modules.contains(vout) ? 0 : 1, module);
    }
    for (const QString &module: modules) {
        const QVector<VlcSupport::Option> options = help.modules.value(module);
        if (options.isEmpty()) continue;
        const QString title = help.moduleNames.value(module).isEmpty() ? module : help.moduleNames.value(module);
        QGroupBox *box = new QGroupBox(title, effectsWidget);
        QVBoxLayout *boxLayout = new QVBoxLayout(box);
        for (const VlcSupport::Option &option: options) {
            OptionRow row;
            row.module = module;
            row.name = option.name;
            row.isInteger = option.isInteger;
            row.isBoolean = option.isBoolean;
            row.isChoice = option.isChoice;
            QHBoxLayout *line = new QHBoxLayout;
            row.set = new QCheckBox(option.description.isEmpty() ? option.name : option.description, box);
            row.set->setToolTip("--" + option.name);
            line->addWidget(row.set, 1);
            // (a change of the value sets the option)
            const auto valueChanged = [this, row]() {
                if (building) return;
                if (!row.set->isChecked()) row.set->setChecked(true);
                updatePreview();
                emit changed();
            };
            if (option.isBoolean) {
                QCheckBox *on = new QCheckBox(tr("on"), box);
                on->setChecked(VlcSupport::defaultOf(option) != 0.0);
                row.value = on;
                connect(on, &QCheckBox::toggled, this, valueChanged);
            } else if (option.isChoice) {
                QComboBox *list = new QComboBox(box);
                for (qsizetype i = 0; i < option.choices.size(); i++) list->addItem(option.choiceLabels.value(i, option.choices[i]), option.choices[i]);
                if (!option.defaultText.isEmpty()) list->setCurrentIndex(qMax(0, list->findData(option.defaultText)));
                row.value = list;
                connect(list, QOverload<int>::of(&QComboBox::currentIndexChanged), this, valueChanged);
            } else if (option.isInteger) {
                QSpinBox *spin = new QSpinBox(box);
                spin->setRange(static_cast<int>(option.minimum), static_cast<int>(option.maximum));
                spin->setValue(static_cast<int>(VlcSupport::defaultOf(option)));
                row.value = spin;
                connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, valueChanged);
            } else {
                QDoubleSpinBox *spin = new QDoubleSpinBox(box);
                spin->setDecimals(3);
                spin->setRange(option.minimum, option.maximum);
                spin->setSingleStep(option.minimum <= -1000000.0 ? 0.1 : qMax(0.001, (option.maximum - option.minimum) / 100.0));
                spin->setValue(VlcSupport::defaultOf(option));
                row.value = spin;
                connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, valueChanged);
            }
            line->addWidget(row.value);
            boxLayout->addLayout(line);
            connect(row.set, &QCheckBox::toggled, this, [this]() {
                if (building) return;
                updatePreview();
                emit changed();
            });
            // (what was set, put back)
            for (const QString &entry: keep) {
                const QString prefix = QString("%1:%2=").arg(module, option.name);
                if (!entry.startsWith(prefix)) continue;
                const QString value = entry.mid(prefix.size());
                const QSignalBlocker blocker(row.value);
                if (option.isBoolean) static_cast<QCheckBox *>(row.value)->setChecked(value == "true");
                else if (option.isChoice) static_cast<QComboBox *>(row.value)->setCurrentIndex(qMax(0, static_cast<QComboBox *>(row.value)->findData(value)));
                else if (option.isInteger) static_cast<QSpinBox *>(row.value)->setValue(value.toInt());
                else static_cast<QDoubleSpinBox *>(row.value)->setValue(value.toDouble());
                const QSignalBlocker setBlocker(row.set);
                row.set->setChecked(true);
            }
            rows.push_back(row);
        }
        effectsLayout->addWidget(box);
    }
    effectsLayout->addStretch(1);
}

void RendererPanel::updatePreview()
{
    const RendererSettings chosen = settings();
    if (chosen.engine == RendererSettings::Vlc) {
        // (VLC is the libvlc of the program: it is started with these arguments, and given the deinterlacing for the Deinterlaced view in place)
        QString mode = chosen.deinterlace.isEmpty() ? QStringLiteral("auto") : chosen.deinterlace;
        if (mode.startsWith(QLatin1String("filter:"))) mode = mode.mid(7);
        previewLabel->setText(tr("libvlc: %1 (deinterlacing in the Deinterlaced view: %2)").arg(chosen.libvlcArguments().join(' '), mode));
        return;
    }
    const QString command = chosen.toCommand(QString());
    previewLabel->setText(command.isEmpty() ? QString() : tr("Command: %1").arg(command));
}
