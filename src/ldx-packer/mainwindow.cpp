/******************************************************************************
 * mainwindow.cpp
 * ldx-packer - the window: the list of captures and the panel with what is known of the one picked
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "mainwindow.h"

#include "appearancemenus.h"
#include "format.h"
#include "addtrackdialog.h"
#include "captureinfo.h"
#include "coverimage.h"
#include "coverwidget.h"
#include "metadatadialog.h"
#include "parametersdialog.h"
#include "processdialog.h"
#include "rfsourcedialog.h"
#include "trackrow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QBuffer>
#include <QDoubleSpinBox>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QInputDialog>
#include <QPixmap>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int kListWidth = 430;
constexpr int kInfoWidth = 470;

QColor trackColor(int k)
{
    return QColor::fromHsv((k * 47) % 360, 190, 215);
}

bool isArchiveName(const QString &path)
{
    return path.endsWith(QStringLiteral(".ldx"), Qt::CaseInsensitive);
}

QString companionText(const Companion &c)
{
    QStringList parts { c.chroma ? QObject::tr("chroma") : Ldx::kindName(c.kind) };
    if (c.depth > 0) parts << QObject::tr("%1 bits").arg(c.depth);
    if (c.sampleRate > 0.0) parts << QObject::tr("%1 Hz").arg(c.sampleRate, 0, 'f', 0);
    if (!c.isNew() && isSamples(c.kind)) parts << Ldx::codecName(c.codec).toUpper();
    QString text = parts.join(QStringLiteral(" · "));
    if (!c.name.isEmpty()) text += QStringLiteral(" (") + c.name + QLatin1Char(')');
    if (c.isNew()) text += QObject::tr(" - new");
    if (c.removed) text += QObject::tr(" - removed");
    if (!c.detail.isEmpty()) text += QStringLiteral("\n") + c.detail;
    return text;
}

} // namespace

MainWindow::MainWindow()
{
    setWindowTitle(tr("LDX-packer"));
    setAcceptDrops(true);
    {
        QSettings settings;
        m_infoShown = settings.value(QStringLiteral("infoShown"), true).toBool();
        m_appearance = new AppearanceMenus(settings.value(QStringLiteral("themeMode"), static_cast<qint32>(ThemeMode::Auto)).toInt(),
                                           settings.value(QStringLiteral("customPalette")).toString(), settings.value(QStringLiteral("styleName")).toString(), this);
    }

    auto *central = new QWidget(this);
    setCentralWidget(central);
    auto *root = new QHBoxLayout(central);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    // The list, as in ld-stacker-gui: a title, the lines in a frame, and the buttons under it.
    auto *left = new QWidget(central);
    left->setMinimumWidth(kListWidth);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(4);

    auto *top = new QHBoxLayout;
    m_processButton = new QPushButton(tr("Create LDX…"), left);
    m_processButton->setObjectName(QStringLiteral("processButton"));
    m_processButton->setToolTip(tr("Write the list as an .ldx file"));
    auto *openButton = new QPushButton(tr("Open LDX…"), left);
    openButton->setObjectName(QStringLiteral("openButton"));
    openButton->setToolTip(tr("Load the captures of an .ldx file into the list"));
    top->addWidget(m_processButton, 1);
    m_updateButton = new QPushButton(tr("Update"), left);
    m_updateButton->setObjectName(QStringLiteral("updateButton"));
    m_updateButton->setToolTip(tr("Write the changes in the opened file itself, with no copy of it: possible when only new metadata were changed. It is greyed when more than that changed (use Process)."));
    top->addWidget(m_updateButton);
    top->addWidget(openButton, 1);
    m_codec = new QComboBox(left);
    m_codec->setObjectName(QStringLiteral("codecBox"));
    m_codec->addItem(tr("As they are"), static_cast<int>(Codec::AsIs));
    m_codec->addItem(tr("Auto"), static_cast<int>(Codec::Auto));
    m_codec->addItem(tr("FLAC"), static_cast<int>(Codec::Flac));
    m_codec->addItem(tr("FFV1"), static_cast<int>(Codec::Ffv1));
    m_codec->addItem(tr("Raw"), static_cast<int>(Codec::Raw));
    m_codec->setToolTip(tr("How the samples are kept: Auto (the best for each track: FLAC for the TBC and the audio, FFV1 for CVBS), FLAC, FFV1 (CVBS only), or Raw "
                           "(as they are, largest). \"As they are\" leaves the tracks of an opened file in the codec they have."));
    top->addWidget(m_codec);
    m_layoutBox = new QComboBox(left);
    m_layoutBox->setObjectName(QStringLiteral("layoutBox"));
    m_layoutBox->addItem(tr("File"), false);
    m_layoutBox->addItem(tr("Stream"), true);
    m_layoutBox->setToolTip(tr("How the file is written: a file with its index (what the tools open at once), or a stream with no index, as a pipe sends it"));
    top->addWidget(m_layoutBox);
    leftLayout->addLayout(top);

    auto *title = new QLabel(tr("Captures"), left);
    title->setForegroundRole(QPalette::PlaceholderText);
    leftLayout->addWidget(title);

    m_listScroll = new QScrollArea(left);
    m_listScroll->setWidgetResizable(true);
    m_listScroll->setFrameShape(QFrame::StyledPanel);
    m_listScroll->setFrameShadow(QFrame::Sunken);
    m_listHost = new QWidget;
    m_listLayout = new QVBoxLayout(m_listHost);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(0);
    m_listScroll->setWidget(m_listHost);
    leftLayout->addWidget(m_listScroll, 1);

    m_hint = new QLabel(tr("<p style=\"font-size:large\">Drag and drop captures or an .ldx file here</p><p>Captures: .tbc .cvbs .cvbsy</p>"), m_listScroll);
    m_hint->setAlignment(Qt::AlignCenter);
    m_hint->setForegroundRole(QPalette::PlaceholderText);

    // The session of the file (session.stack): what it names, to be put in, taken out or replaced
    m_sessionBox = new QFrame(left);
    m_sessionBox->setObjectName(QStringLiteral("sessionBox"));
    m_sessionBox->setFrameShape(QFrame::StyledPanel);
    auto *sessionLayout = new QHBoxLayout(m_sessionBox);
    m_sessionText = new QLabel(m_sessionBox);
    m_sessionText->setObjectName(QStringLiteral("sessionText"));
    m_sessionText->setWordWrap(true);
    m_sessionText->setTextFormat(Qt::RichText);
    sessionLayout->addWidget(m_sessionText, 1);
    m_sessionSet = new QPushButton(m_sessionBox);
    m_sessionSet->setObjectName(QStringLiteral("sessionSet"));
    m_sessionDrop = new QPushButton(m_sessionBox);
    m_sessionDrop->setObjectName(QStringLiteral("sessionDrop"));
    sessionLayout->addWidget(m_sessionSet, 0, Qt::AlignTop);
    sessionLayout->addWidget(m_sessionDrop, 0, Qt::AlignTop);
    leftLayout->addWidget(m_sessionBox);

    m_summary = new QLabel(left);
    m_summary->setObjectName(QStringLiteral("summary"));
    m_summary->setForegroundRole(QPalette::PlaceholderText);
    leftLayout->addWidget(m_summary);

    auto *buttons = new QHBoxLayout;
    auto *addButton = new QPushButton(tr("Add capture…"), left);
    addButton->setObjectName(QStringLiteral("addButton"));
    addButton->setToolTip(tr("Add captures at the end of the list"));
    m_infoButton = new QPushButton(tr("Info"), left);
    m_infoButton->setCheckable(true);
    m_infoButton->setChecked(m_infoShown);
    m_infoButton->setToolTip(tr("Show or hide the panel with what is known of the selected capture (key I)"));
    m_verifyAll = new QPushButton(tr("Check"), left);
    m_verifyAll->setObjectName(QStringLiteral("verifyAll"));
    m_verifyAll->setToolTip(tr("Check the whole file: every block is read in the order of the file and by its place in its track, and the two must agree"));
    buttons->addWidget(addButton, 1);
    buttons->addWidget(m_verifyAll);
    buttons->addWidget(m_infoButton);
    leftLayout->addLayout(buttons);
    root->addWidget(left);

    // The panel on the right: all that is known of the capture picked. The name of one that is to be added, the files that go with
    // it and the notes can be changed there.
    m_info = new QWidget(central);
    m_info->setObjectName(QStringLiteral("infoPanel"));
    m_info->setFixedWidth(kInfoWidth);
    auto *infoLayout = new QVBoxLayout(m_info);
    infoLayout->setContentsMargins(0, 0, 0, 0);
    infoLayout->setSpacing(4);
    // The cover of the file, above what is known of the capture: the picture whole in a frame that fits it, and what can be done with it
    m_coverBox = new QFrame(m_info);
    m_coverBox->setObjectName(QStringLiteral("coverBox"));
    m_coverBox->setFrameShape(QFrame::StyledPanel);
    m_coverBox->setFrameShadow(QFrame::Sunken);
    m_coverBox->setBackgroundRole(QPalette::Base);
    m_coverBox->setAutoFillBackground(true);
    auto *coverLayout = new QVBoxLayout(m_coverBox);
    coverLayout->setContentsMargins(6, 6, 6, 6);
    coverLayout->setSpacing(4);
    m_coverImage = new CoverWidget(m_coverBox);
    m_coverImage->setRoom(QSize(kInfoWidth - 34, 240));
    coverLayout->addWidget(m_coverImage, 0, Qt::AlignHCenter);
    auto *coverRow = new QHBoxLayout;
    m_coverText = new QLabel(m_coverBox);
    m_coverText->setObjectName(QStringLiteral("coverText"));
    m_coverText->setForegroundRole(QPalette::PlaceholderText);
    m_coverText->setWordWrap(true);
    coverRow->addWidget(m_coverText, 1);
    m_coverSet = new QPushButton(m_coverBox);
    m_coverSet->setObjectName(QStringLiteral("coverSet"));
    m_coverDrop = new QPushButton(m_coverBox);
    m_coverDrop->setObjectName(QStringLiteral("coverDrop"));
    coverLayout->addLayout(coverRow);
    auto *coverButtons = new QHBoxLayout;
    m_coverSource = new QPushButton(tr("Address…"), m_coverBox);
    m_coverSource->setObjectName(QStringLiteral("coverSource"));
    m_coverSource->setToolTip(tr("Keep the address of a picture (a web address, or a path of a file, relative to the folder of the .ldx file) instead of the picture itself. "
                                 "A web address is fetched only when someone asks to see the picture."));
    m_coverLink = new QPushButton(tr("Link…"), m_coverBox);
    m_coverLink->setObjectName(QStringLiteral("coverLink"));
    m_coverLink->setToolTip(tr("A web address that opens in the browser when the cover is clicked"));
    coverButtons->addWidget(m_coverSet);
    coverButtons->addWidget(m_coverSource);
    coverButtons->addWidget(m_coverLink);
    coverButtons->addWidget(m_coverDrop);
    coverLayout->addLayout(coverButtons);
    infoLayout->addWidget(m_coverBox);

    auto *infoHead = new QHBoxLayout;
    auto *infoLabel = new QLabel(tr("Info"), m_info);
    infoLabel->setForegroundRole(QPalette::PlaceholderText);
    infoHead->addWidget(infoLabel);
    infoHead->addStretch();
    infoLayout->addLayout(infoHead);

    auto *infoScroll = new QScrollArea(m_info);
    infoScroll->setWidgetResizable(true);
    infoScroll->setFrameShape(QFrame::StyledPanel);
    infoScroll->setFrameShadow(QFrame::Sunken);
    infoScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *infoBody = new QWidget;
    auto *body = new QVBoxLayout(infoBody);
    body->setContentsMargins(8, 8, 8, 8);
    body->setSpacing(6);
    m_infoTitle = new QLabel(infoBody);
    m_infoTitle->setObjectName(QStringLiteral("infoTitle"));
    m_infoTitle->setWordWrap(true);
    body->addWidget(m_infoTitle);

    auto *form = new QGridLayout;
    form->setColumnStretch(1, 1);
    auto *nameLabel = new QLabel(tr("Name"), infoBody);
    nameLabel->setForegroundRole(QPalette::PlaceholderText);
    m_name = new QLineEdit(infoBody);
    m_name->setObjectName(QStringLiteral("infoName"));
    form->addWidget(nameLabel, 0, 0);
    form->addWidget(m_name, 0, 1);
    auto *aspectLabel = new QLabel(tr("Shape"), infoBody);
    aspectLabel->setForegroundRole(QPalette::PlaceholderText);
    m_aspect = new QComboBox(infoBody);
    m_aspect->setObjectName(QStringLiteral("infoAspect"));
    m_aspect->addItem(tr("Not set (1:1)"), QString());
    m_aspect->addItem(tr("4:3"), QStringLiteral("4:3"));
    m_aspect->addItem(tr("16:9"), QStringLiteral("16:9"));
    m_aspect->setToolTip(tr("The shape the picture is shown in by video players (VLC, ffplay): 4:3 or 16:9 sets the display aspect ratio in the header of the video track (the same as ld-analyse shows the capture in) and "
                            "isWidescreen in the metadata with it (ld-analyse reads that as it did). "
                            "Not set shows the whole image with square pixels (1:1)."));
    form->addWidget(aspectLabel, 1, 0);
    form->addWidget(m_aspect, 1, 1);
    auto *systemLabel = new QLabel(tr("System"), infoBody);
    systemLabel->setForegroundRole(QPalette::PlaceholderText);
    m_system = new QComboBox(infoBody);
    m_system->setObjectName(QStringLiteral("infoSystem"));
    m_system->addItem(QStringLiteral("PAL"), QStringLiteral("PAL"));
    m_system->addItem(QStringLiteral("NTSC"), QStringLiteral("NTSC"));
    m_system->addItem(QStringLiteral("PAL-M"), QStringLiteral("PAL-M"));
    m_system->addItem(QStringLiteral("SECAM"), QStringLiteral("SECAM"));
    m_system->addItem(QStringLiteral("NTSC-VHD"), QStringLiteral("NTSC-VHD"));
    m_system->setToolTip(tr("The system of the capture (a parameter of its video track and of its metadata: the length of a field, the shape of the picture and the decoder come from it). SECAM has the 625 lines of PAL and NTSC-VHD the 525 lines of NTSC; no decoder makes colour of SECAM."));
    form->addWidget(systemLabel, 2, 0);
    form->addWidget(m_system, 2, 1);
    auto *videoRateLabel = new QLabel(tr("Sample rate"), infoBody);
    videoRateLabel->setForegroundRole(QPalette::PlaceholderText);
    m_videoRate = new QDoubleSpinBox(infoBody);
    m_videoRate->setObjectName(QStringLiteral("infoVideoRate"));
    m_videoRate->setDecimals(3);
    m_videoRate->setRange(100000.0, 1000000000.0);
    m_videoRate->setSuffix(tr(" Hz"));
    m_videoRate->setToolTip(tr("The sample rate of the video, in Hz (4 times the subcarrier for the standard captures)"));
    form->addWidget(videoRateLabel, 3, 0);
    form->addWidget(m_videoRate, 3, 1);
    // What a person says of the capture (specification 3.1 and 8.1): said by a person, never guessed; "not said" takes the value out of the file
    const auto saidLabel = [infoBody](const QString &text) {
        auto *label = new QLabel(text, infoBody);
        label->setForegroundRole(QPalette::PlaceholderText);
        return label;
    };
    m_medium = new QComboBox(infoBody);
    m_medium->setObjectName(QStringLiteral("infoMedium"));
    m_medium->addItem(tr("Not said"), QString());
    m_medium->addItem(tr("LaserDisc"), QStringLiteral("ld"));
    m_medium->addItem(tr("VHS tape"), QStringLiteral("vhs"));
    m_medium->addItem(tr("VideoDisc (VHD)"), QStringLiteral("vhd"));
    m_medium->addItem(tr("Composite signal (CVBS)"), QStringLiteral("cvbs"));
    m_medium->setToolTip(tr("What was captured, as you know it (it is not guessed). A LaserDisc's work (the TBC curve, the stack) is done for a LaserDisc."));
    form->addWidget(saidLabel(tr("Medium")), 4, 0);
    form->addWidget(m_medium, 4, 1);
    m_reference = new QLineEdit(infoBody);
    m_reference->setObjectName(QStringLiteral("infoReference"));
    m_reference->setPlaceholderText(tr("a catalogue number, a title..."));
    form->addWidget(saidLabel(tr("Reference")), 6, 0);
    form->addWidget(m_reference, 6, 1);
    m_copy = new QSpinBox(infoBody);
    m_copy->setObjectName(QStringLiteral("infoCopy"));
    m_copy->setRange(0, 9999);
    m_copy->setSpecialValueText(tr("Not said"));
    m_copy->setToolTip(tr("The number of the physical copy of the disc (the exemplar) the capture was made from"));
    form->addWidget(saidLabel(tr("Disc copy")), 7, 0);
    form->addWidget(m_copy, 7, 1);
    m_captureNumber = new QSpinBox(infoBody);
    m_captureNumber->setObjectName(QStringLiteral("infoCaptureNumber"));
    m_captureNumber->setRange(0, 9999);
    m_captureNumber->setSpecialValueText(tr("Not said"));
    m_captureNumber->setToolTip(tr("The number of the capture itself (one copy can be captured several times, so it is not the number of the copy)"));
    form->addWidget(saidLabel(tr("Capture no.")), 8, 0);
    form->addWidget(m_captureNumber, 8, 1);
    m_face = new QComboBox(infoBody);
    m_face->setObjectName(QStringLiteral("infoFace"));
    m_face->addItem(tr("Not said"), QString());
    for (const QString &face : CaptureInfo::faces()) m_face->addItem(face, face);
    m_face->setToolTip(tr("The face of the disc: A, B, or both (AB, BA: in that order in the capture)"));
    form->addWidget(saidLabel(tr("Face")), 9, 0);
    form->addWidget(m_face, 9, 1);
    body->addLayout(form);
    m_aspectWarning = new QLabel(infoBody);
    m_aspectWarning->setObjectName(QStringLiteral("aspectWarning"));
    m_aspectWarning->setWordWrap(true);
    m_aspectWarning->setTextFormat(Qt::RichText);
    m_aspectWarning->hide();
    body->addWidget(m_aspectWarning);
    m_aspectFix = new QPushButton(infoBody);
    m_aspectFix->setObjectName(QStringLiteral("aspectFix"));
    m_aspectFix->hide();
    body->addWidget(m_aspectFix);

    m_facts = new QLabel(infoBody);
    m_facts->setObjectName(QStringLiteral("infoFacts"));
    m_facts->setTextFormat(Qt::RichText);
    m_facts->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_facts->setWordWrap(true);
    body->addWidget(m_facts);

    auto *companionsLabel = new QLabel(tr("Tracks that go with it"), infoBody);
    companionsLabel->setForegroundRole(QPalette::PlaceholderText);
    body->addWidget(companionsLabel);
    m_companions = new QListWidget(infoBody);
    m_companions->setObjectName(QStringLiteral("infoCompanions"));
    m_companions->setMinimumHeight(64);
    m_companions->setWordWrap(true);
    m_companions->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_companions->setTextElideMode(Qt::ElideNone);
    // (each track a block of its own, with a line between them: two lines of text each would read as one text otherwise)
    m_companions->setStyleSheet(QStringLiteral("QListWidget#infoCompanions::item { padding: 5px 4px; border-bottom: 1px solid rgba(128, 128, 128, 120); }"));
    body->addWidget(m_companions);
    // The track picked in the list: its name and, for audio, its sample rate (the file is changed in its place when it can be)
    m_trackBox = new QFrame(infoBody);
    m_trackBox->setObjectName(QStringLiteral("trackBox"));
    m_trackBox->setFrameShape(QFrame::StyledPanel);
    m_trackBox->setFrameShadow(QFrame::Sunken);
    // (the colour of the lists and the fields, so that it is a hollow in the panel and not a block of its own colour)
    m_trackBox->setBackgroundRole(QPalette::Base);
    m_trackBox->setAutoFillBackground(true);
    auto *trackLayout = new QGridLayout(m_trackBox);
    m_trackInfo = new QLabel(m_trackBox);
    m_trackInfo->setObjectName(QStringLiteral("trackInfo"));
    m_trackInfo->setWordWrap(true);
    trackLayout->addWidget(m_trackInfo, 0, 0, 1, 2);
    auto *trackNameLabel = new QLabel(tr("Name"), m_trackBox);
    trackNameLabel->setForegroundRole(QPalette::PlaceholderText);
    m_trackName = new QLineEdit(m_trackBox);
    m_trackName->setObjectName(QStringLiteral("trackName"));
    trackLayout->addWidget(trackNameLabel, 1, 0);
    trackLayout->addWidget(m_trackName, 1, 1);
    m_trackRateLabel = new QLabel(tr("Sample rate"), m_trackBox);
    m_trackRateLabel->setForegroundRole(QPalette::PlaceholderText);
    m_trackRate = new QDoubleSpinBox(m_trackBox);
    m_trackRate->setObjectName(QStringLiteral("trackRate"));
    m_trackRate->setDecimals(0);
    m_trackRate->setRange(8000.0, 192000.0);
    m_trackRate->setSuffix(tr(" Hz"));
    m_trackRate->setToolTip(tr("The rate the samples of the audio are played at (44100 Hz for the audio of a capture). A player plays them at the rate that is declared, so a wrong "
                               "one makes the sound too fast or too slow. FLAC audio is coded at its rate, so changing it writes the file again."));
    trackLayout->addWidget(m_trackRateLabel, 2, 0);
    trackLayout->addWidget(m_trackRate, 2, 1);
    m_trackWarning = new QLabel(m_trackBox);
    m_trackWarning->setObjectName(QStringLiteral("trackWarning"));
    m_trackWarning->setWordWrap(true);
    m_trackWarning->setTextFormat(Qt::RichText);
    trackLayout->addWidget(m_trackWarning, 4, 0, 1, 2);
    m_trackFix = new QPushButton(m_trackBox);
    m_trackFix->setObjectName(QStringLiteral("trackFix"));
    trackLayout->addWidget(m_trackFix, 5, 0, 1, 2);
    m_trackParameters = new QPushButton(tr("Parameters…"), m_trackBox);
    m_trackParameters->setObjectName(QStringLiteral("trackParameters"));
    m_trackParameters->setToolTip(tr("All the parameters of this track, as JSON"));
    trackLayout->addWidget(m_trackParameters, 3, 1, Qt::AlignRight);
    m_trackBox->hide();
    auto *companionButtons = new QHBoxLayout;
    m_addCompanion = new QPushButton(tr("Add…"), infoBody);
    m_addCompanion->setObjectName(QStringLiteral("addCompanion"));
    m_addCompanion->setToolTip(tr("Add a file as a track about this capture: audio (.pcm, .wav), EFM (.efm), AC3 (.ac3) or notes (a text), with its name and, for audio, its sample rate"));
    m_removeCompanion = new QPushButton(tr("Remove"), infoBody);
    m_removeCompanion->setObjectName(QStringLiteral("removeCompanion"));
    companionButtons->addWidget(m_addCompanion);
    companionButtons->addWidget(m_removeCompanion);
    m_metadata = new QPushButton(tr("Metadata…"), infoBody);
    m_metadata->setObjectName(QStringLiteral("editMetadata"));
    m_metadata->setToolTip(tr("Look at the metadata of this capture, change its parameters, or replace them with those of a .json or .db file"));
    companionButtons->addWidget(m_metadata);
    m_rfSource = new QPushButton(tr("Where from…"), infoBody);
    m_rfSource->setObjectName(QStringLiteral("editRfSource"));
    m_rfSource->setToolTip(tr("Where the capture comes from: what is known of the RF file it was decoded from (its name, size, rate, bits, date, the files beside it). Information to trace the capture, with no other use"));
    companionButtons->addWidget(m_rfSource);
    m_videoParameters = new QPushButton(tr("Parameters…"), infoBody);
    m_videoParameters->setObjectName(QStringLiteral("videoParameters"));
    m_videoParameters->setToolTip(tr("All the parameters of the video track of this capture, as JSON (what the tools know of it: the system, the sample rate, the size of a field...)"));
    companionButtons->addWidget(m_videoParameters);
    companionButtons->addStretch();
    body->addLayout(companionButtons);
    // (the buttons are just under the list they are about, and the box of the picked track under them)
    body->addWidget(m_trackBox);

    auto *notesLabel = new QLabel(tr("Notes"), infoBody);
    notesLabel->setForegroundRole(QPalette::PlaceholderText);
    body->addWidget(notesLabel);
    m_notes = new QPlainTextEdit(infoBody);
    m_notes->setObjectName(QStringLiteral("infoNotes"));
    m_notes->setPlaceholderText(tr("A free note, kept in the file as a track of annotations"));
    m_notes->setMinimumHeight(40);
    body->addWidget(m_notes, 1);
    m_deleteNotes = new QPushButton(tr("Delete note"), infoBody);
    m_deleteNotes->setObjectName(QStringLiteral("deleteNotes"));
    auto *noteButtons = new QHBoxLayout;
    noteButtons->addStretch();
    noteButtons->addWidget(m_deleteNotes);
    body->addLayout(noteButtons);
    infoScroll->setWidget(infoBody);
    infoLayout->addWidget(infoScroll, 1);
    root->addWidget(m_info);

    connect(&m_doc, &Document::changed, this, [this] {
        if (m_rebuildQueued) return;
        m_rebuildQueued = true;
        QTimer::singleShot(0, this, [this] {
            m_rebuildQueued = false;
            rebuildList();
            if (!m_editing) refreshInfo();
        });
    });
    connect(m_codec, &QComboBox::currentIndexChanged, this, [this](int) { m_doc.setCodec(static_cast<Codec>(m_codec->currentData().toInt())); });
    connect(m_layoutBox, &QComboBox::activated, this, [this](int index) {
        const bool stream = m_layoutBox->itemData(index).toBool();
        if (stream && !m_doc.stream()) {
            const auto answer = QMessageBox::warning(
                this, tr("Write as a stream"),
                tr("<b>A stream has no index.</b><br><br>"
                   "It is written as a pipe sends it: nothing is written again, so it has no cues. A program that opens it has to read it all once to find the "
                   "blocks, and the ones that read a pipe cannot jump about in it.<br><br>"
                   "A tool that needs the metadata of every field before it starts (stacking, dropout correction between sources) refuses a stream that sends them "
                   "one field at a time. The attachments, the session included, are only at the start.<br><br>"
                   "Keep a file with its index for an archive; use a stream to pass the capture on to another program.<br><br>"
                   "Write it as a stream?"),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Yes) {
                syncCodec();
                return;
            }
        }
        m_doc.setStream(stream);
    });
    connect(m_processButton, &QPushButton::clicked, this, [this] { process(); });
    connect(m_updateButton, &QPushButton::clicked, this, [this] {
        if (m_doc.canUpdate()) process(m_doc.archive(), QSettings().value(QStringLiteral("packVerify"), true).toBool());
    });
    connect(openButton, &QPushButton::clicked, this, &MainWindow::chooseArchive);
    connect(addButton, &QPushButton::clicked, this, &MainWindow::chooseCaptures);
    connect(m_infoButton, &QPushButton::toggled, this, [this](bool on) { showInfo(on); });
    connect(m_name, &QLineEdit::textEdited, this, &MainWindow::editName);
    connect(m_notes, &QPlainTextEdit::textChanged, this, &MainWindow::editNotes);
    connect(m_deleteNotes, &QPushButton::clicked, this, [this] { m_notes->clear(); });
    connect(m_addCompanion, &QPushButton::clicked, this, &MainWindow::addCompanion);
    connect(m_removeCompanion, &QPushButton::clicked, this, &MainWindow::removeCompanion);
    connect(m_metadata, &QPushButton::clicked, this, &MainWindow::editMetadata);
    connect(m_rfSource, &QPushButton::clicked, this, &MainWindow::editRfSource);
    connect(m_videoParameters, &QPushButton::clicked, this, [this] { editParameters(true); });
    connect(m_trackParameters, &QPushButton::clicked, this, [this] { editParameters(false); });
    connect(m_trackFix, &QPushButton::clicked, this, [this] {
        const int row = m_companions->currentRow();
        if (m_picked < 0 || row < 0) return;
        m_doc.edit(m_picked).companions[static_cast<size_t>(row)].declareRate = true;
        m_doc.touched();
    });
    connect(m_aspectFix, &QPushButton::clicked, this, [this] {
        if (m_picked < 0 || m_aspectFixTo.isEmpty()) return;
        Entry &entry = m_doc.edit(m_picked);
        entry.aspect = m_aspectFixTo;
        entry.forceDisplay = true;
        m_doc.touched();
    });
    connect(m_aspect, &QComboBox::activated, this, [this](int index) {
        if (m_picked < 0) return;
        m_doc.edit(m_picked).aspect = m_aspect->itemData(index).toString();
        m_doc.touched();
    });
    // What a person says of the capture: the parameters of the video track (the file is changed in its place when it can be)
    const auto saysParameter = [this](const QString &key, const QJsonValue &value) {
        if (m_picked < 0 || m_refreshingSaid) return;
        Entry &entry = m_doc.edit(m_picked);
        if (value.isNull() || (value.isString() && value.toString().isEmpty()) || (value.isDouble() && value.toInt() == 0)) entry.videoParameters.remove(key);
        else entry.videoParameters[key] = value;
        m_doc.touched();
    };
    connect(m_medium, &QComboBox::activated, this, [this, saysParameter](int index) { saysParameter(QStringLiteral("medium"), m_medium->itemData(index).toString()); });
    connect(m_face, &QComboBox::activated, this, [this, saysParameter](int index) { saysParameter(QStringLiteral("face"), m_face->itemData(index).toString()); });
    connect(m_reference, &QLineEdit::editingFinished, this, [this, saysParameter]() { saysParameter(QStringLiteral("reference"), m_reference->text().trimmed()); });
    connect(m_copy, &QSpinBox::editingFinished, this, [this, saysParameter]() { saysParameter(QStringLiteral("copy"), m_copy->value()); });
    connect(m_captureNumber, &QSpinBox::editingFinished, this, [this, saysParameter]() { saysParameter(QStringLiteral("capture"), m_captureNumber->value()); });
    connect(m_sessionSet, &QPushButton::clicked, this, &MainWindow::chooseSession);
    connect(m_coverSet, &QPushButton::clicked, this, &MainWindow::chooseCover);
    connect(m_coverSource, &QPushButton::clicked, this, &MainWindow::chooseCoverSource);
    connect(m_coverLink, &QPushButton::clicked, this, &MainWindow::chooseCoverLink);
    connect(m_coverDrop, &QPushButton::clicked, this, [this] {
        // (the same button undoes what was chosen, or takes the cover out)
        if (m_doc.coverEdited()) m_doc.undoCover();
        else {
            m_doc.removeCover();
            m_doc.setCoverSource(QString());
        }
    });
    connect(m_sessionDrop, &QPushButton::clicked, this, [this] {
        // (the same button takes the session out, or gives it back when it was to be taken out or replaced)
        if (m_doc.sessionChange() != SessionChange::Keep) m_doc.keepSession();
        else m_doc.removeSession();
    });
    connect(m_verifyAll, &QPushButton::clicked, this, [this] { verify(); });
    connect(m_companions, &QListWidget::currentRowChanged, this, [this](int row) {
        m_removeCompanion->setEnabled(row >= 0);
        if (row >= 0 && row != m_pickedTrack) {
            m_pickedTrack = row;
            QTimer::singleShot(0, this, [this] { rebuildList(); });
        }
        refreshTrackEditor();
    });
    connect(m_system, &QComboBox::activated, this, [this](int index) {
        if (m_picked < 0) return;
        m_doc.edit(m_picked).videoParameters["system"] = m_system->itemData(index).toString();
        m_doc.touched();
    });
    connect(m_videoRate, &QDoubleSpinBox::editingFinished, this, [this] {
        if (m_picked < 0 || m_doc.at(m_picked).videoParameters.value("sampleRate").toDouble() == m_videoRate->value()) return;
        m_doc.edit(m_picked).videoParameters["sampleRate"] = m_videoRate->value();
        m_doc.touched();
    });
    connect(m_trackName, &QLineEdit::textEdited, this, [this](const QString &text) {
        const int row = m_companions->currentRow();
        if (m_picked < 0 || row < 0) return;
        Companion &c = m_doc.edit(m_picked).companions[static_cast<size_t>(row)];
        c.name = text;
        if (auto *item = m_companions->item(row)) item->setText(companionText(c));
        m_editing = true;
        m_doc.touched();
        m_editing = false;
    });
    connect(m_trackRate, &QDoubleSpinBox::editingFinished, this, [this] {
        const int row = m_companions->currentRow();
        if (m_picked < 0 || row < 0) return;
        Companion &c = m_doc.edit(m_picked).companions[static_cast<size_t>(row)];
        if (c.parameters.value("sampleRate").toDouble() == m_trackRate->value()) return;
        c.parameters["sampleRate"] = m_trackRate->value();
        c.sampleRate = m_trackRate->value();
        if (auto *item = m_companions->item(row)) item->setText(companionText(c));
        m_editing = true;
        m_doc.touched();
        m_editing = false;
    });

    buildMenus();
    m_info->setVisible(m_infoShown);
    resize(kListWidth + 16 + (m_infoShown ? kInfoWidth + 8 : 0), 860);
    syncCodec();
    rebuildList();
    refreshInfo();
}

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Create LDX from a new list"), this, &MainWindow::newList)->setShortcut(QKeySequence::New);
    file->addAction(tr("&Open LDX…"), this, &MainWindow::chooseArchive)->setShortcut(QKeySequence::Open);
    file->addAction(tr("&Add capture…"), this, &MainWindow::chooseCaptures)->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
    file->addSeparator();
    m_unpackAction = file->addAction(tr("&Unpack all the tracks…"), this, [this] { unpack(); });
    m_unpackAction->setObjectName(QStringLiteral("unpackAll"));
    m_verifyAction = file->addAction(tr("&Check the file"), this, [this] { verify(); });
    m_verifyAction->setObjectName(QStringLiteral("verifyFile"));
    m_verifyAction->setToolTip(tr("Read every block of the file in the order of the file and by its place in its track: the two must agree"));
    m_historyAction = file->addAction(tr("Show the &history…"), this, &MainWindow::showHistory);
    m_historyAction->setObjectName(QStringLiteral("showHistory"));
    connect(file, &QMenu::aboutToShow, this, [this] {
        const bool opened = !m_doc.archive().isEmpty();
        m_unpackAction->setEnabled(opened);
        m_verifyAction->setEnabled(opened);
        m_historyAction->setEnabled(opened);
    });
    file->addSeparator();
    file->addAction(tr("&Quit"), this, &QWidget::close)->setShortcut(QKeySequence::Quit);

    QMenu *view = menuBar()->addMenu(tr("&View"));
    m_infoAction = view->addAction(tr("&Info panel"));
    m_infoAction->setCheckable(true);
    m_infoAction->setChecked(m_infoShown);
    m_infoAction->setShortcut(QKeySequence(Qt::Key_I));
    connect(m_infoAction, &QAction::toggled, this, [this](bool on) { showInfo(on); });
    view->addSeparator();
    connect(m_appearance, &AppearanceMenus::themeModeStored, this, [](qint32 mode) { QSettings().setValue(QStringLiteral("themeMode"), mode); });
    connect(m_appearance, &AppearanceMenus::customPaletteStored, this, [](const QString &palette) { QSettings().setValue(QStringLiteral("customPalette"), palette); });
    connect(m_appearance, &AppearanceMenus::styleNameStored, this, [](const QString &name) { QSettings().setValue(QStringLiteral("styleName"), name); });
    connect(m_appearance, &AppearanceMenus::changed, this, [] {
        for (QWidget *widget : QApplication::allWidgets()) widget->update();
    });
    connect(m_appearance, &AppearanceMenus::customizeRequested, this, [this](QWidget *parent) { m_appearance->customize(parent, QApplication::palette()); });
    m_appearance->install(view);

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About LDX-packer"), this, [this] {
        QMessageBox::about(this, tr("About LDX-packer"),
                           tr("<b>LDX-packer</b> %1<br>Makes an .ldx file from captures, and changes the captures of one: add, remove, keep the samples in another codec, "
                              "add audio, EFM and AC3 tracks and notes.<br>Part of ld-decode-tools, GPL-3.0-or-later.")
                               .arg(QCoreApplication::applicationVersion()));
    });
}

void MainWindow::showInfo(bool shown)
{
    if (shown == m_infoShown) return;
    const int before = width();
    m_infoShown = shown;
    QSettings().setValue(QStringLiteral("infoShown"), shown);
    const QSignalBlocker a(m_infoButton);
    const QSignalBlocker b(m_infoAction);
    m_infoButton->setChecked(shown);
    m_infoAction->setChecked(shown);
    m_info->setVisible(shown);
    centralWidget()->layout()->activate();
    layout()->activate();
    // The window grows by the width of the panel, and gets it back when the panel goes (unless it is maximised).
    if (!isMaximized() && !isFullScreen()) {
        const int delta = kInfoWidth + 8;
        resize(std::max(kListWidth + 16, before + (shown ? delta : -delta)), height());
    }
}

// The selector of the codec shows what the list says; a new list cannot leave the codecs "as they are"
void MainWindow::syncCodec()
{
    const QSignalBlocker blocker(m_codec);
    m_codec->setCurrentIndex(m_codec->findData(static_cast<int>(m_doc.codec())));
    if (auto *model = qobject_cast<QStandardItemModel *>(m_codec->model())) {
        model->item(0)->setEnabled(!m_doc.archive().isEmpty());
    }
    const QSignalBlocker layoutBlocker(m_layoutBox);
    m_layoutBox->setCurrentIndex(m_doc.stream() ? 1 : 0);
}

void MainWindow::rebuildList()
{
    syncCodec();
    while (QLayoutItem *item = m_listLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    const int n = m_doc.count();
    if (m_picked >= n) m_picked = n - 1;
    for (int i = 0; i < n; ++i) {
        const Entry &shown = m_doc.at(i);
        const QString key = shown.fromArchive ? QString::number(shown.video.number) : shown.capture;
        const bool expandable = !shown.companions.empty() || !shown.notes.isEmpty();
        const bool expanded = expandable && !m_collapsed.contains(key);
        auto *row = new TrackRow(shown, i + 1, m_doc.state(i), trackColor(i), i == m_picked, m_doc.canMove(i, -1), m_doc.canMove(i, 1), expandable, expanded, m_listHost);
        connect(row, &TrackRow::expandToggled, this, [this, key](int) {
            if (!m_collapsed.remove(key)) m_collapsed.insert(key);
            rebuildList();
        });
        connect(row, &TrackRow::picked, this, [this](int number) { pick(number - 1); });
        connect(row, &TrackRow::moved, this, [this](int number, int delta) { move(number - 1, delta); });
        connect(row, &TrackRow::removeAsked, this, [this](int number) { removeCapture(number - 1); });
        connect(row, &TrackRow::restoreAsked, this, [this](int number) { m_doc.restore(number - 1); });
        m_listLayout->addWidget(row);
        // The tracks that go with it, under it: the metadata, the audio, the EFM, the AC3 and the notes, each a line that is picked to be changed in the panel
        if (expanded) {
            const Entry &entry = m_doc.at(i);
            for (size_t j = 0; j < entry.companions.size(); ++j) {
                auto *child = new TrackChildRow(entry.companions[j], i, static_cast<int>(j), trackColor(i), i == m_picked && m_pickedTrack == static_cast<int>(j), m_listHost);
                connect(child, &TrackChildRow::picked, this, [this](int capture, int track) { pickTrack(capture, track); });
                connect(child, &TrackChildRow::removeAsked, this, [this](int capture, int track) { removeTrack(capture, track); });
                m_listLayout->addWidget(child);
            }
            if (!entry.notes.isEmpty()) {
                auto *child = new TrackChildRow(entry.notes, i, trackColor(i), i == m_picked && m_pickedTrack == -1, m_listHost);
                connect(child, &TrackChildRow::picked, this, [this](int capture, int track) { pickTrack(capture, track); });
                m_listLayout->addWidget(child);
            }
        }
    }
    m_listLayout->addStretch(1);
    m_hint->setVisible(n == 0);
    m_hint->setGeometry(m_listScroll->viewport()->rect());
    m_processButton->setText(m_doc.archive().isEmpty() ? tr("Create LDX…") : tr("Process…"));
    m_verifyAll->setEnabled(!m_doc.archive().isEmpty());
    refreshSummary();
    refreshSession();
    refreshCover();
}

void MainWindow::refreshSummary()
{
    const Summary s = m_doc.summary();
    QStringList parts;
    parts << tr("%n capture(s)", nullptr, s.kept + s.added);
    if (s.added) parts << tr("%1 new").arg(s.added);
    if (s.removed) parts << tr("%1 removed").arg(s.removed);
    if (s.recoded) parts << tr("%1 to recode").arg(s.recoded);
    if (s.edited) parts << tr("%1 edited").arg(s.edited);
    if (!m_doc.archive().isEmpty()) {
        parts << fmt::bytes(m_doc.archiveSize());
        parts << (m_doc.hasIndex() ? tr("file with an index") : tr("stream, no index"));
    }
    m_summary->setText(parts.join(QStringLiteral(" · ")));
    m_processButton->setEnabled(m_doc.count() > 0 && (s.kept + s.added) > 0);
    m_updateButton->setEnabled(m_doc.canUpdate());
    setWindowTitle(m_doc.archive().isEmpty() ? tr("LDX-packer") : tr("LDX-packer - %1").arg(QFileInfo(m_doc.archive()).fileName()));
}

void MainWindow::pick(int index)
{
    if (index < 0 || index >= m_doc.count()) index = -1;
    // (picking the line of a capture leaves the track that was picked under it)
    if (index != m_picked || m_pickedTrack != -2) {
        m_pickedTrack = -2;
        if (index == m_picked) {
            m_companions->setCurrentRow(-1);
            rebuildList();
            return;
        }
    } else {
        return;
    }
    m_picked = index;
    rebuildList();
    refreshInfo();
}

// A track under a capture picked in the list: the capture is picked, and the track is the one the panel shows
void MainWindow::pickTrack(int capture, int row)
{
    if (capture < 0 || capture >= m_doc.count()) return;
    if (capture != m_picked) {
        m_pickedTrack = -2;
        m_picked = capture;
        rebuildList();
        refreshInfo();
    }
    m_pickedTrack = row;
    if (row >= 0 && row < m_companions->count()) m_companions->setCurrentRow(row);
    else if (row == -1) m_notes->setFocus();
    rebuildList();
}

void MainWindow::refreshInfo()
{
    const bool has = m_picked >= 0 && m_picked < m_doc.count();
    const QSignalBlocker a(m_name);
    const QSignalBlocker b(m_notes);
    const QSignalBlocker c(m_companions);
    const int keepRow = m_companions->currentRow();
    m_companions->clear();
    m_name->setEnabled(false);
    m_notes->setEnabled(has);
    m_deleteNotes->setEnabled(has);
    m_addCompanion->setEnabled(has);
    m_removeCompanion->setEnabled(false);
    m_metadata->setEnabled(has);
    m_rfSource->setEnabled(has);
    m_videoParameters->setEnabled(has && m_doc.at(m_picked).fromArchive);
    m_aspect->setEnabled(has);
    if (!has) {
        for (QWidget *widget : { static_cast<QWidget *>(m_medium), static_cast<QWidget *>(m_reference), static_cast<QWidget *>(m_copy),
                                 static_cast<QWidget *>(m_captureNumber), static_cast<QWidget *>(m_face) }) widget->setEnabled(false);
        m_infoTitle->setText(tr("<i>No capture selected</i>"));
        const QSignalBlocker shapeOff(m_aspect);
        m_aspect->setCurrentIndex(0);
        m_name->clear();
        m_notes->clear();
        m_facts->clear();
        m_trackBox->hide();
        m_aspectWarning->hide();
        m_aspectFix->hide();
        return;
    }
    const Entry &e = m_doc.at(m_picked);
    {
        const QSignalBlocker shapeBlocker(m_aspect);
        m_aspect->setCurrentIndex(std::max(0, m_aspect->findData(e.aspect)));
    }
    // What a person said of the capture
    {
        m_refreshingSaid = true;
        const bool editable = true;       // (a capture that is to be added is made with what is said: ld-container pack and add)
        for (QWidget *widget : { static_cast<QWidget *>(m_medium), static_cast<QWidget *>(m_reference), static_cast<QWidget *>(m_copy),
                                 static_cast<QWidget *>(m_captureNumber), static_cast<QWidget *>(m_face) }) widget->setEnabled(editable);
        m_medium->setCurrentIndex(std::max(0, m_medium->findData(e.videoParameters.value("medium").toString())));
        m_face->setCurrentIndex(std::max(0, m_face->findData(e.videoParameters.value("face").toString())));
        if (m_reference->text() != e.videoParameters.value("reference").toString() && !m_reference->hasFocus()) m_reference->setText(e.videoParameters.value("reference").toString());
        m_copy->setValue(e.videoParameters.value("copy").toInt(0));
        m_captureNumber->setValue(e.videoParameters.value("capture").toInt(0));
        m_refreshingSaid = false;
    }
    // What is wrong with the shape in the header of the track (only when 4:3 or 16:9 was chosen: 1:1 stays 1:1), and the way to make it right
    {
        QString fix;
        const QString problem = m_doc.displayProblem(m_picked, &fix);
        m_aspectFixTo = fix;
        if (e.forceDisplay) {
            m_aspectWarning->setText(tr("The header will say the shape <b>%1</b> when the file is written.").arg(e.aspect.toHtmlEscaped()));
            m_aspectWarning->show();
            m_aspectFix->hide();
        } else if (!problem.isEmpty()) {
            m_aspectWarning->setText(QStringLiteral("<b>%1</b>").arg(problem.toHtmlEscaped()));
            m_aspectWarning->show();
            m_aspectFix->setText(tr("Make the header say %1 (as the metadata)").arg(fix));
            m_aspectFix->show();
        } else {
            m_aspectWarning->hide();
            m_aspectFix->hide();
        }
    }
    m_name->setEnabled(true);
    m_name->setToolTip(tr("What the capture is called in the file (the name of its video track)"));
    m_infoTitle->setText(QStringLiteral("<b>%1</b>").arg(e.name.toHtmlEscaped()));
    m_name->setText(e.name);
    if (m_notes->toPlainText() != e.notes) m_notes->setPlainText(e.notes);
    for (const Companion &comp : e.companions) {
        auto *item = new QListWidgetItem(companionText(comp), m_companions);
        if (comp.removed) {
            QFont font = item->font();
            font.setStrikeOut(true);
            item->setFont(font);
        }
        if (!comp.file.isEmpty()) item->setToolTip(QDir::toNativeSeparators(comp.file));
        else if (!comp.detail.isEmpty()) item->setToolTip(comp.detail);
    }
    if (keepRow >= 0 && keepRow < m_companions->count()) m_companions->setCurrentRow(keepRow);
    {
        const QSignalBlocker systemBlocker(m_system);
        const QSignalBlocker rateBlocker(m_videoRate);
        const bool inFile = e.fromArchive;
        m_system->setEnabled(inFile);
        m_videoRate->setEnabled(inFile);
        if (inFile) {
            m_system->setCurrentIndex(std::max(0, m_system->findData(e.videoParameters.value("system").toString())));
            m_videoRate->setValue(e.videoParameters.value("sampleRate").toDouble(14318181.818));
        } else {
            // (a capture that is to be added: its system and rate are read from its metadata when it is packed)
            m_system->setCurrentIndex(-1);
            m_videoRate->setSpecialValueText(QStringLiteral("—"));
            m_videoRate->setValue(m_videoRate->minimum());
        }
    }

    QStringList rows;
    const auto add = [&rows](const QString &k, const QString &v) {
        rows << QStringLiteral("<tr><td style=\"padding-right:10px\"><small>%1</small></td><td>%2</td></tr>").arg(k.toHtmlEscaped(), v.toHtmlEscaped());
    };
    if (e.fromArchive || !e.ldxFile.isEmpty()) {
        if (!e.ldxFile.isEmpty()) add(tr("From"), QDir::toNativeSeparators(e.ldxFile));
        const QJsonObject &p = e.video.parameters;
        const bool cvbs = e.video.kind == Ldx::Kind::Cvbs;
        add(tr("Kind"), cvbs ? tr("CVBS") : tr("TBC"));
        if (p.contains("fieldWidth")) add(tr("Field size"), QStringLiteral("%1 × %2").arg(p.value("fieldWidth").toInt()).arg(p.value("fieldHeight").toInt()));
        add(cvbs ? tr("Frames") : tr("Fields"), fmt::count(e.blocks));
        if (e.seconds > 0.0) add(tr("Duration"), fmt::duration(e.seconds));
        add(tr("Kept as"), QStringLiteral("%1 · %2 bits").arg(Ldx::codecName(e.video.codec).toUpper()).arg(e.video.depth));
        add(tr("Track"), QString::number(e.video.number));
        if (p.contains("mappedFrom")) add(tr("Mapping of"), tr("track %1").arg(p.value("mappedFrom").toInt()));
        if (e.chroma.number != 0) add(tr("Chroma"), tr("track %1 · %2").arg(e.chroma.number).arg(Ldx::codecName(e.chroma.codec).toUpper()));
    } else {
        add(tr("File"), QDir::toNativeSeparators(e.capture));
        add(tr("Size"), fmt::bytes(QFileInfo(e.capture).size()));
    }
    m_facts->setText(QStringLiteral("<table>%1</table>").arg(rows.join(QString())));
    refreshTrackEditor();
}

// The track picked in the list of what goes with the capture: what it is, its name, and for audio its sample rate
void MainWindow::refreshTrackEditor()
{
    const int row = m_companions->currentRow();
    if (m_picked < 0 || m_picked >= m_doc.count() || row < 0 || row >= static_cast<int>(m_doc.at(m_picked).companions.size())) {
        m_trackBox->hide();
        return;
    }
    const Companion &c = m_doc.at(m_picked).companions[static_cast<size_t>(row)];
    if (c.isNew() || c.removed) {
        m_trackBox->hide();
        return;
    }
    QStringList facts { tr("Track %1").arg(c.number), c.chroma ? tr("chroma of the Y/C capture") : Ldx::kindName(c.kind) };
    if (c.depth > 0) facts << tr("%1 bits").arg(c.depth);
    if (c.kind == Ldx::Kind::Pcm && c.parameters.contains("channels")) facts << tr("%1 channels").arg(c.parameters.value("channels").toInt());
    if (isSamples(c.kind)) facts << Ldx::codecName(c.codec).toUpper();
    m_trackInfo->setText(facts.join(QStringLiteral(" · ")));
    if (m_trackName->text() != c.name) m_trackName->setText(c.name);
    const bool audio = c.kind == Ldx::Kind::Pcm;
    m_trackRateLabel->setVisible(audio);
    m_trackRate->setVisible(audio);
    if (audio) {
        const QSignalBlocker rateBlocker(m_trackRate);
        m_trackRate->setValue(c.parameters.value("sampleRate").toDouble(44100.0));
    }
    // The rate a player plays the audio at is the one in the header of the track: when it is not the one of the parameters (a file written before the header
    // said the real rate), the sound is played too fast or too slow
    const bool mismatch = c.rateDiffers();
    m_trackWarning->setVisible(mismatch || c.declareRate);
    m_trackFix->setVisible(mismatch && !c.declareRate);
    if (c.declareRate) {
        m_trackWarning->setText(tr("The header will say <b>%1 Hz</b> when the file is written.").arg(c.parameters.value("sampleRate").toDouble(), 0, 'f', 0));
    } else if (mismatch) {
        m_trackWarning->setText(tr("<b>The header says %1 Hz</b>, and a player (ffplay, VLC) plays the audio at that rate, while the track says %2 Hz: the sound is %3.")
                                    .arg(c.headerRate, 0, 'f', 0).arg(c.parameters.value("sampleRate").toDouble(), 0, 'f', 0)
                                    .arg(c.headerRate > c.parameters.value("sampleRate").toDouble() ? tr("too fast") : tr("too slow")));
        m_trackFix->setText(tr("Declare %1 Hz in the header").arg(c.parameters.value("sampleRate").toDouble(), 0, 'f', 0));
    }
    m_trackBox->show();
}

void MainWindow::editName(const QString &text)
{
    if (m_picked < 0) return;
    m_editing = true;
    m_doc.edit(m_picked).name = text;
    m_infoTitle->setText(QStringLiteral("<b>%1</b>").arg(text.toHtmlEscaped()));
    m_doc.touched();
    m_editing = false;
}

void MainWindow::editNotes()
{
    if (m_picked < 0) return;
    m_editing = true;
    m_doc.edit(m_picked).notes = m_notes->toPlainText();
    m_doc.touched();
    m_editing = false;
}

// Add a file to the capture picked as a track: the dialog asks what it is, its name and, for audio, its rate (the files that are dropped on the window are
// added at once, as what their names say)
void MainWindow::addCompanion()
{
    if (m_picked < 0) return;
    AddTrackDialog dialog(m_doc.at(m_picked).name, QString(), this);
    if (dialog.exec() != QDialog::Accepted) return;
    QString error;
    const bool added = dialog.kind() == Ldx::Kind::Tbc ? m_doc.addChroma(m_picked, dialog.file(), dialog.name(), &error)
                                                       : m_doc.addTrack(m_picked, dialog.file(), dialog.kind(), dialog.name(), dialog.sampleRate(), &error);
    if (!added) {
        QMessageBox::warning(this, tr("Add"), tr("%1 cannot be added: %2").arg(QFileInfo(dialog.file()).fileName(), error));
    }
}

void MainWindow::editMetadata()
{
    if (m_picked < 0 || m_picked >= m_doc.count()) return;
    MetadataDialog dialog(m_doc, m_picked, this);
    if (dialog.ok()) dialog.exec();
}

// All the parameters of the video track of the capture picked, or of the track picked under it, as JSON
void MainWindow::editParameters(bool video)
{
    if (m_picked < 0 || m_picked >= m_doc.count()) return;
    Entry &entry = m_doc.edit(m_picked);
    const int row = m_companions->currentRow();
    if (!video && (row < 0 || row >= static_cast<int>(entry.companions.size()))) return;
    Companion *track = video ? nullptr : &entry.companions[static_cast<size_t>(row)];
    ParametersDialog dialog(video ? tr("Parameters of %1").arg(entry.name) : tr("Parameters of %1").arg(track->name), video ? entry.videoParameters : track->parameters, this);
    if (dialog.exec() != QDialog::Accepted) return;
    if (video) {
        entry.videoParameters = dialog.parameters();
    } else {
        track->parameters = dialog.parameters();
        track->sampleRate = track->parameters.value("sampleRate").toDouble();
    }
    m_doc.touched();
}

// The cover of the file above the information: the picture whole in a frame that fits it (any shape: from 1:1 to 16:9 it is what the frame is made for)
void MainWindow::refreshCover()
{
    const bool listed = m_doc.count() > 0;
    m_coverBox->setVisible(listed);
    if (!listed) return;
    const CoverChange change = m_doc.coverChange();
    const QByteArray &data = m_doc.cover();
    const QString source = m_doc.coverSource();
    const QString link = m_doc.coverLink();
    m_coverImage->setContent(data, source, link, m_doc.archive());
    QStringList facts;
    if (!data.isEmpty()) {
        const QImage picture = CoverImage::fromData(data);
        facts << (picture.isNull() ? tr("this picture cannot be read") : tr("%1 × %2").arg(picture.width()).arg(picture.height())) << fmt::bytes(data.size());
    }
    QString text;
    if (!data.isEmpty()) text = tr("Cover · %1").arg(facts.join(QStringLiteral(" · ")));
    else if (!source.isEmpty()) text = tr("Cover kept at %1").arg(source);
    else text = tr("Cover: none");
    if (change == CoverChange::Set) text += tr(" · to be put in the file");
    else if (change == CoverChange::Remove) text = tr("Cover: to be taken out of the file");
    if (!link.isEmpty()) text += QStringLiteral("\n") + tr("Link: %1").arg(link);
    m_coverText->setText(text);
    const bool hasAny = !data.isEmpty() || !source.isEmpty();
    m_coverSet->setText(hasAny ? tr("Change…") : tr("Add…"));
    m_coverSet->setToolTip(tr("Choose a picture of the capture, the disc or the tape to be the cover of the file: it is shown whole and scaled to at most 800 pixels high"));
    m_coverLink->setText(link.isEmpty() ? tr("Link…") : tr("Link ✎"));
    m_coverDrop->setVisible(hasAny || m_doc.coverEdited() || !link.isEmpty());
    m_coverDrop->setText(m_doc.coverEdited() ? tr("Undo") : tr("Remove"));
}

// The address of a picture kept outside the file (a web address, or a path: relative to the folder of the file)
void MainWindow::chooseCoverSource()
{
    bool ok = false;
    const QString text = QInputDialog::getText(this, tr("Cover of the file"),
                                               tr("Address of the picture (http or https), or path of a file, relative to the folder of the .ldx file:"),
                                               QLineEdit::Normal, m_doc.coverSource(), &ok).trimmed();
    if (!ok) return;
    if (text.contains(QLatin1String("://")) && !text.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        && !text.startsWith(QLatin1String("https://"), Qt::CaseInsensitive) && !text.startsWith(QLatin1String("file://"), Qt::CaseInsensitive)) {
        QMessageBox::warning(this, tr("Cover"), tr("A web address starts with http:// or https://."));
        return;
    }
    // (the picture kept in the file is taken out when there is one: the address stands in its place)
    if (!text.isEmpty() && !m_doc.cover().isEmpty()) m_doc.removeCover();
    m_doc.setCoverSource(text);
}

// The link that opens when the cover is clicked
void MainWindow::chooseCoverLink()
{
    bool ok = false;
    const QString text = QInputDialog::getText(this, tr("Link of the cover"), tr("Web address that opens when the cover is clicked (empty for none):"),
                                               QLineEdit::Normal, m_doc.coverLink(), &ok).trimmed();
    if (!ok) return;
    if (!text.isEmpty() && !text.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) && !text.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
        QMessageBox::warning(this, tr("Cover"), tr("A link is a web address: http:// or https://."));
        return;
    }
    m_doc.setCoverLink(text);
}

// Choose a picture: it is scaled to at most 800 pixels high, keeping its shape, and kept as a PNG (or as a JPEG when it was one and this build writes them)
void MainWindow::chooseCover()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Cover of the file"), QSettings().value(QStringLiteral("lastDir")).toString(),
                                                      tr("Pictures (*.png *.jpg *.jpeg *.webp *.gif *.bmp);;All files (*)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Cover"), tr("%1 cannot be opened.").arg(QFileInfo(path).fileName()));
        return;
    }
    QByteArray cover;
    QString suffix, error;
    if (!CoverImage::prepare(file.readAll(), cover, suffix, &error)) {
        QMessageBox::warning(this, tr("Cover"), tr("%1 cannot be used as a cover: %2").arg(QFileInfo(path).fileName(), error));
        return;
    }
    QSettings().setValue(QStringLiteral("lastDir"), QFileInfo(path).absolutePath());
    m_doc.setCover(cover, suffix);
}

void MainWindow::chooseSession()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Session of a stacking"), QSettings().value(QStringLiteral("lastDir")).toString(),
                                                      tr("Sessions (*.stack);;All files (*)"));
    if (path.isEmpty()) return;
    bool embed = false;
    if (QFileInfo(path).isFile()) {
        // The sources of the session that are .tbc files can be brought into the file, as tracks of their own
        embed = QMessageBox::question(this, tr("Session"),
                                      tr("Bring the sources of the session that are .tbc files into the file, as captures? (Those that are tracks of the file, "
                                         "or links to other files, are kept as they are.)"),
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
    }
    QString error;
    if (!m_doc.setSession(path, embed, &error)) {
        QMessageBox::warning(this, tr("Session"), tr("%1 cannot be used: %2").arg(QFileInfo(path).fileName(), error));
        return;
    }
    QSettings().setValue(QStringLiteral("lastDir"), QFileInfo(path).absolutePath());
}

void MainWindow::refreshSession()
{
    const bool listed = m_doc.count() > 0;
    m_sessionBox->setVisible(listed);
    if (!listed) return;
    const SessionChange change = m_doc.sessionChange();
    QString text;
    if (change == SessionChange::Set) {
        text = tr("<b>Session</b> (session.stack): the one of %1 is to be put in the file%2.").arg(QFileInfo(m_doc.sessionFile()).fileName().toHtmlEscaped(),
                                                                                                       m_doc.sessionEmbeds() ? tr(", its .tbc files with it") : QString());
    } else if (change == SessionChange::Remove) {
        text = tr("<b>Session</b> (session.stack): to be taken out of the file. The captures stay.");
    } else if (m_doc.hasSession()) {
        QStringList lines;
        for (const QString &line : m_doc.sessionLines()) lines << line.toHtmlEscaped();
        text = tr("<b>Session</b> (session.stack) with %n source(s):", nullptr, static_cast<int>(lines.size())) + QStringLiteral("<br>") + lines.join(QStringLiteral("<br>"));
    } else {
        text = tr("<b>Session</b>: none. A stacking session (a .stack file) can be kept in the file, its sources being captures of it.");
    }
    m_sessionText->setText(text);
    m_sessionSet->setText(m_doc.hasSession() || change == SessionChange::Set ? tr("Replace…") : tr("Add…"));
    m_sessionSet->setToolTip(tr("Choose a .stack file to keep in the file (the session it has is replaced)"));
    m_sessionDrop->setVisible(m_doc.hasSession() || change != SessionChange::Keep);
    m_sessionDrop->setText(change != SessionChange::Keep ? tr("Keep") : tr("Remove"));
    m_sessionDrop->setToolTip(change != SessionChange::Keep ? tr("Leave the session as it is in the file") : tr("Take the session out of the file (the captures stay)"));
}

// Take a track that goes with a capture out of the file (or keep it again); the metadata of a capture are what its tracks are read with, so that is asked
void MainWindow::removeTrack(int capture, int row)
{
    if (capture < 0 || capture >= m_doc.count() || row < 0 || row >= static_cast<int>(m_doc.at(capture).companions.size())) return;
    const Companion &c = m_doc.at(capture).companions[static_cast<size_t>(row)];
    if (c.kind == Ldx::Kind::Metadata && c.parameters.value("namespace").toString() != QLatin1String("ldx.rfsource") && !c.removed && !c.isNew()) {
        const auto answer = QMessageBox::question(this, tr("Remove"),
                                                  tr("Without its metadata a capture has no fields to read, no system and no dropouts: the tools can only show its samples. Take the metadata of %1 out of the file?")
                                                      .arg(m_doc.at(capture).name),
                                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
    }
    m_doc.removeCompanion(capture, row);
}

void MainWindow::removeCompanion()
{
    removeTrack(m_picked, m_companions->currentRow());
}

void MainWindow::move(int index, int delta)
{
    m_doc.move(index, delta);
    // The picked line follows the capture it is on.
    if (m_picked == index) m_picked = index + delta;
    else if (m_picked == index + delta) m_picked = index;
}

void MainWindow::removeCapture(int index)
{
    m_doc.remove(index);
    if (m_picked >= m_doc.count()) m_picked = m_doc.count() - 1;
}

bool MainWindow::confirmDiscard()
{
    if (!m_doc.isChanged()) return true;
    const auto answer = QMessageBox::question(this, tr("LDX-packer"), tr("The list has changes that are not written (captures added, removed or moved, files that go with a capture, notes, how the samples are kept). Discard them?"));
    return answer == QMessageBox::Yes;
}

void MainWindow::newList()
{
    if (!confirmDiscard()) return;
    m_picked = -1;
    m_doc.clear();
}

void MainWindow::chooseArchive()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Open an .ldx file"), QSettings().value(QStringLiteral("lastDir")).toString(), tr("LDX files (*.ldx)"));
    if (!path.isEmpty()) openArchive(path);
}

void MainWindow::chooseCaptures()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Add captures"), QSettings().value(QStringLiteral("lastDir")).toString(),
                                                            tr("Captures, their audio and EFM, and .ldx files (*.tbc *.cvbs *.cvbsy *.pcm *.wav *.efm *.ac3 *.ldx);;All files (*)"));
    if (!paths.isEmpty()) addCaptures(paths);
}

bool MainWindow::openArchive(const QString &path)
{
    if (!confirmDiscard()) return false;
    QString error;
    if (!m_doc.open(path, &error)) {
        QMessageBox::warning(this, tr("Open"), tr("%1 cannot be opened: %2").arg(QFileInfo(path).fileName(), error));
        return false;
    }
    QSettings().setValue(QStringLiteral("lastDir"), QFileInfo(path).absolutePath());
    m_picked = m_doc.count() > 0 ? 0 : -1;
    return true;
}

bool MainWindow::addCaptures(const QStringList &paths)
{
    bool all = true;
    for (const QString &path : paths) {
        QString error;
        static const QStringList companionSuffixes { QStringLiteral("pcm"), QStringLiteral("wav"), QStringLiteral("efm"), QStringLiteral("ac3"), QStringLiteral("db"), QStringLiteral("json") };
        if (companionSuffixes.contains(QFileInfo(path).suffix().toLower())) {
            // An audio, EFM or AC3 file goes with a capture: the one that has its name, or else the one that is picked
            int target = m_picked;
            for (int i = 0; i < m_doc.count(); ++i) {
                if (QFileInfo(m_doc.at(i).name).completeBaseName() == QFileInfo(path).completeBaseName()) target = i;
            }
            if (target < 0 || target >= m_doc.count()) {
                error = tr("it goes with a capture: add the capture first, or pick it in the list");
            } else if (m_doc.addCompanion(target, path, &error)) {
                continue;
            }
        } else if (isArchiveName(path)) {
            // The captures of another .ldx file are put in this one, each with its tracks (when the list is empty it is opened instead)
            if (m_doc.count() == 0 && m_doc.archive().isEmpty()) {
                if (openArchive(path)) continue;
                all = false;
                continue;
            }
            if (m_doc.addArchive(path, &error)) continue;
        } else if (m_doc.addCapture(path, &error)) {
            continue;
        }
        QMessageBox::warning(this, tr("Add capture"), tr("%1 cannot be added: %2").arg(QFileInfo(path).fileName(), error));
        all = false;
    }
    if (!paths.isEmpty()) {
        QSettings().setValue(QStringLiteral("lastDir"), QFileInfo(paths.first()).absolutePath());
        if (m_picked < 0 && m_doc.count() > 0) m_picked = 0;
    }
    return all;
}

bool MainWindow::runTool(const QString &title, const QStringList &arguments, QString *lastLine, bool quiet)
{
    Plan plan;
    plan.directory = std::make_shared<QTemporaryDir>();
    plan.output = m_doc.archive();
    Step step;
    step.description = title;
    step.program = QStringLiteral("ld-container");
    step.arguments = [arguments] { return arguments; };
    plan.steps.append(step);
    ProgressDialog progress(plan, this);
    progress.setWindowTitle(title);
    progress.start();
    if (progress.exec() != QDialog::Accepted) {
        if (!progress.error().isEmpty() && !quiet) QMessageBox::critical(this, tr("LDX-packer"), progress.error());
        return false;
    }
    if (lastLine) *lastLine = progress.lastLine();
    return true;
}

bool MainWindow::process(const QString &output, bool verifyAfter)
{
    QString target = output;
    if (target.isEmpty()) {
        ProcessDialog dialog(m_doc.archive(), this);
        if (dialog.exec() != QDialog::Accepted) return false;
        target = dialog.output();
        verifyAfter = dialog.verifies();
    }
    Plan plan = m_doc.toPlan(target, verifyAfter);
    if (plan.steps.isEmpty()) {
        QMessageBox::information(this, tr("LDX-packer"), tr("There is nothing to write: the list is as the file is."));
        return false;
    }
    ProgressDialog progress(plan, this);
    progress.start();
    if (progress.exec() != QDialog::Accepted) {
        if (!progress.error().isEmpty()) QMessageBox::critical(this, tr("LDX-packer"), progress.error());
        return false;
    }
    statusBar()->showMessage(tr("%1 written").arg(QFileInfo(target).fileName()), 8000);
    // What was written is what the list is now.
    const int keep = m_picked;
    m_doc.open(target, nullptr);
    m_picked = std::min(keep, m_doc.count() - 1);
    return true;
}

bool MainWindow::unpack(const QString &folder)
{
    if (m_doc.archive().isEmpty()) return false;
    QString target = folder;
    if (target.isEmpty()) {
        QSettings settings;
        const QString start = settings.value(QStringLiteral("unpackDir"), QFileInfo(m_doc.archive()).absolutePath()).toString();
        target = QFileDialog::getExistingDirectory(this, tr("Unpack all the tracks into…"), start);
        if (target.isEmpty()) return false;
        settings.setValue(QStringLiteral("unpackDir"), target);
    }
    if (!runTool(tr("Unpacking into %1").arg(QFileInfo(target).fileName()), { QStringLiteral("unpack"), m_doc.archive(), QStringLiteral("-o"), target })) return false;
    statusBar()->showMessage(tr("Unpacked into %1").arg(QDir::toNativeSeparators(target)), 8000);
    return true;
}

bool MainWindow::verify(bool quiet)
{
    if (m_doc.archive().isEmpty()) return false;
    QString last;
    if (!runTool(tr("Checking %1").arg(QFileInfo(m_doc.archive()).fileName()), { QStringLiteral("verify"), m_doc.archive() }, &last, quiet)) return false;
    if (!quiet) QMessageBox::information(this, tr("Check"), last);
    statusBar()->showMessage(last, 8000);
    return true;
}

void MainWindow::showHistory()
{
    QStringList lines;
    int number = 0;
    for (const QJsonValue &value : m_doc.history()) {
        const QJsonObject entry = value.toObject();
        const QString via = entry.value("via").toString();
        QString line = QStringLiteral("%1. %2  %3%4  %5").arg(++number).arg(entry.value("time").toString(), entry.value("tool").toString(), via.isEmpty() ? QString() : tr(" (by %1)").arg(via), entry.value("command").toString());
        if (entry.contains("detail")) line += QStringLiteral(": ") + entry.value("detail").toString();
        lines << line;
    }
    QMessageBox box(QMessageBox::Information, tr("History"), lines.isEmpty() ? tr("The history is empty.") : tr("What was done to %1:").arg(QFileInfo(m_doc.archive()).fileName()), QMessageBox::Close, this);
    box.setDetailedText(lines.join(QLatin1Char('\n')));
    box.exec();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList captures;
    QStringList archives;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        const QString path = url.toLocalFile();
        (isArchiveName(path) ? archives : captures) << path;
    }
    // The first .ldx file dropped on an empty list is opened (the others are refused, as are the ones dropped on a list that has captures).
    QStringList more = archives;
    if (m_doc.count() == 0 && !more.isEmpty()) {
        if (!openArchive(more.takeFirst())) return;
    }
    addCaptures(more + captures);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (confirmDiscard()) event->accept();
    else event->ignore();
}

bool MainWindow::saveScreenshot(const QString &path)
{
    show();
    QApplication::processEvents();
    QTimer::singleShot(0, [] {});
    QApplication::processEvents();
    return grab().save(path);
}

// Where the capture comes from (the track ldx.rfsource): what is known of the RF file it was decoded from
void MainWindow::editRfSource()
{
    if (m_picked < 0 || m_picked >= m_doc.count()) return;
    RfSourceDialog dialog(m_doc.at(m_picked).rfSource, m_doc.at(m_picked).name, this);
    if (dialog.exec() != QDialog::Accepted) return;
    const QByteArray json = dialog.json();
    if (json == m_doc.at(m_picked).rfSource) return;
    m_doc.edit(m_picked).rfSource = json;
    m_doc.touched();
}
