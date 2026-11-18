/******************************************************************************
 * document.h
 * ldx-packer - the list of captures being made: those of an .ldx file and the ones added to them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef DOCUMENT_H
#define DOCUMENT_H

#include "ldx.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <cmath>
#include <functional>
#include <memory>
#include <vector>

class QTemporaryDir;

// What goes with a capture: a track about it (its metadata, audio, EFM, AC3, notes), or a file that is to become one.
struct Companion
{
    // The number of its track in the file (0 for a file that is to be added).
    quint32 number = 0;
    Ldx::Kind kind = Ldx::Kind::Unknown;
    Ldx::Codec codec = Ldx::Codec::Raw;
    int depth = 0;
    QString name;
    // The file it is made from, for one that is to be added.
    QString file;
    double sampleRate = 0.0;
    bool removed = false;
    // What the track holds, in a few words (read when the file is opened): for metadata the way they are kept and what they hold.
    QString detail;
    // The name and the parameters of the track as the file has them (originalName, originalParameters) and as they are now (name, parameters): the sample rate of
    // audio, what is changed in the file is the difference.
    QString originalName;
    QJsonObject parameters;
    QJsonObject originalParameters;
    // The sampling frequency the header of an audio track declares (what a player plays it at), 0 if it is not known; declareRate: the header is to be written
    // again with the rate of the parameters (when the two differ, as in a file written before the header said the real rate)
    double headerRate = 0.0;
    bool declareRate = false;
    // The chroma of a Y/C capture (a track of the same kind as the capture, with the role chroma, about its luma); beside: it is the file that is beside the capture to be
    // added, which ld-container pack takes with it (unless it is left out)
    bool chroma = false;
    bool beside = false;

    bool rateDiffers() const { return kind == Ldx::Kind::Pcm && headerRate > 0.0 && std::abs(headerRate - parameters.value("sampleRate").toDouble()) > 0.5; }

    bool isNew() const { return number == 0; }
};

// One line of the list: a capture of the .ldx file that was opened (its video track, with the tracks about it), or a capture that is to be added.
struct Entry
{
    bool fromArchive = false;
    // The video track (with its number), and the chroma track of a Y/C capture (number 0: none).
    Ldx::Track video;
    Ldx::Track chroma;
    // How much of it the file holds.
    qint64 blocks = 0;
    double seconds = 0.0;
    std::vector<Companion> companions;
    // A capture that is to be added: its file (the .tbc or the .cvbs), and what it is called in the file.
    QString capture;
    QString name;
    // The notes (a track of annotations), as the file has them and as they are now; the number of the track that has them (0: none).
    QString notes;
    QString originalNotes;
    quint32 notesTrack = 0;
    // New metadata for the capture (the JSON of a .json or .db file, or the current ones edited); empty: it keeps the ones it has.
    QByteArray newMetadata;
    // The shape the capture is shown in by video players: "4:3", "16:9", or empty (not set: 1:1); as the file has it and as it is now.
    QString aspect;
    // A capture taken from another .ldx file (ldxFile, and its video track there): its tracks are the ones the file has, to be put in this one (ld-container merge)
    QString ldxFile;
    quint32 ldxTrack = 0;
    bool besideChroma = false;              // a capture to be added: it has a chroma file beside it (Y/C), which pack takes unless the line of it was taken out
    int metadataWide = -1;                  // isWidescreen of the metadata of the capture (1, 0), -1 when they do not say
    bool forceDisplay = false;              // the header of the video track is to be written again for its shape, though it was not changed (to match the metadata)
    QJsonObject videoParameters;            // the parameters of the video track as they are now (system, sample rate...), and as the file has them (video.parameters)
    QString originalAspect;
    // Where the capture comes from (the track ldx.rfsource: the JSON of what is known of the RF file it was decoded from, compact), as the file has it and as it is now; empty: none
    QByteArray rfSource;
    QByteArray originalRfSource;
    bool removed = false;
};

enum class EntryState { Kept, Edited, Recode, New, Removed };

// What is to be done with the cover of the file (section 9 of the specification): nothing, take it out, or put a picture in its place.
enum class CoverChange { Keep, Remove, Set };

// What is to be done with the session (session.stack) of the file: nothing, take it out, or put the one of a .stack file in its place.
enum class SessionChange { Keep, Remove, Set };

struct Summary
{
    int kept = 0;
    int added = 0;
    int removed = 0;
    int recoded = 0;
    int edited = 0;
};

// How the samples are kept in the file (what ld-container --codec says). AsIs leaves what the file has; a list that is new does not have it.
enum class Codec { AsIs, Auto, Flac, Ffv1, Raw };

QString codecArgument(Codec codec);

// One piece of the work: a command of ld-container, or something the window does itself (the file that is done takes its name).
struct Step
{
    QString description;
    // The program (ld-container), and its arguments, given when the step starts: what an earlier step made is known by then.
    QString program;
    std::function<QStringList()> arguments;
    // What the program said, once it is done.
    std::function<void(const QString &)> after;
    // For a step that is not a program.
    std::function<bool(QString *)> action;
};

struct Plan
{
    QList<Step> steps;
    QString output;
    // Where the files made on the way are: taken away with it, at the end or when the work stops.
    std::shared_ptr<QTemporaryDir> directory;
};

class Document : public QObject
{
    Q_OBJECT

public:
    explicit Document(QObject *parent = nullptr) : QObject(parent) {}

    // Load the captures of an .ldx file (the list starts from them). False, with the reason, when it is not one.
    bool open(const QString &path, QString *error);
    // Add a capture at the end, with the audio, EFM and AC3 files that sit beside it. False, with the reason, when it cannot be.
    bool addCapture(const QString &path, QString *error);
    // Add the captures of another .ldx file at the end, each with the tracks that go with it (they are put in the file when it is written). False, with the reason.
    bool addArchive(const QString &path, QString *error);
    // An empty list, not made from a file.
    void clear();

    Codec codec() const { return m_codec; }
    void setCodec(Codec codec);

    // The file the list was opened from (empty for a new list, even when captures were added to it).
    QString archive() const { return m_archive; }
    qint64 archiveSize() const { return m_archiveSize; }
    // Whether the file has an index (cues): one that was written as a stream and saved as it came has none.
    bool hasIndex() const { return m_hasIndex; }
    // How the file is to be written: as a stream (no index) or as a file with its index. An opened file starts as it is.
    bool stream() const { return m_stream; }
    void setStream(bool stream);
    const QJsonArray &history() const { return m_history; }
    int count() const { return static_cast<int>(m_entries.size()); }
    const Entry &at(int i) const { return m_entries[static_cast<size_t>(i)]; }
    Entry &edit(int i) { return m_entries[static_cast<size_t>(i)]; }

    void remove(int i);
    void restore(int i);
    // Move a capture (the file is then written with the tracks numbered again to follow the list).
    bool canMove(int i, int delta) const;
    void move(int i, int delta);
    // Add a file to a capture, as a track about it (.pcm, .efm, .ac3); false, with the reason, when it is not one of those.
    bool addCompanion(int i, const QString &file, QString *error);
    // Add a file to a capture as a track of a kind that is chosen: audio (raw 16-bit stereo, or a .wav), EFM t-values, AC3 data, or notes (a text). False, with the reason.
    bool addTrack(int i, const QString &file, Ldx::Kind kind, const QString &name, double sampleRate, QString *error);
    // Add the chroma file of a Y/C capture (_chroma.tbc, .tbcc, .cvbsc) to a capture that has no chroma; false, with the reason, when it cannot be
    bool addChroma(int i, const QString &file, const QString &name, QString *error);
    // The sample rate of a .wav file of 16-bit stereo PCM (false, with the reason, when it is not one)
    static bool wavRate(const QString &path, double *rate, QString *error);
    void removeCompanion(int i, int which);
    // The metadata of a capture, as JSON: the ones it has (those of the file, or of the files beside a capture that is to be added), or the new ones.
    bool metadataOf(int i, QJsonObject *root, QString *error) const;
    // The metadata of a .json or .db file of ld-decode, as JSON.
    static bool readMetadataFile(const QString &path, QJsonObject *root, QString *error);
    // Keep new metadata for a capture (written to the file with the others: the new ones are the current ones from then on).
    void setMetadata(int i, const QJsonObject &root);
    void clearMetadata(int i);

    // The session of the file (session.stack): what it names, one line for each source, and what is to be done with it.
    bool hasSession() const { return m_hasSession; }
    const QStringList &sessionLines() const { return m_sessionLines; }
    SessionChange sessionChange() const { return m_sessionChange; }
    QString sessionFile() const { return m_sessionFile; }
    bool sessionEmbeds() const { return m_sessionEmbed; }
    // Put the session of a .stack file in the file (embed: the sources that are .tbc files are brought in as tracks); false, with the reason, when it is not one.
    bool setSession(const QString &path, bool embed, QString *error);
    void removeSession();
    void keepSession();

    // The cover of the file: the picture it has (or the one that is to be put in), as PNG or JPEG data, and what is to be done with it. The picture is scaled to at most 800
    // pixels high by whoever gives it (the window does), as the specification says.
    const QByteArray &cover() const { return m_coverChange == CoverChange::Set ? m_coverNew : (m_coverChange == CoverChange::Remove ? m_none : m_cover); }
    CoverChange coverChange() const { return m_coverChange; }
    // Whether the file as it was opened has a cover
    bool fileHasCover() const { return !m_cover.isEmpty(); }
    bool hasCover() const { return !cover().isEmpty(); }
    void setCover(const QByteArray &image, const QString &suffix);
    void removeCover();
    void keepCover();
    // Where the picture is kept outside the file (cover.url: a web address, or a path, absolute or relative to the folder of the file; empty: none), and the link that opens when the
    // cover is clicked (link.url); as they are now, and whether they differ from what the file has. Everything about the cover is given back with undoCover.
    const QString &coverSource() const { return m_coverSource; }
    const QString &coverLink() const { return m_coverLink; }
    void setCoverSource(const QString &source);
    void setCoverLink(const QString &link);
    bool coverReferenceChanged() const { return m_coverSource != m_coverSourceOriginal || m_coverLink != m_coverLinkOriginal; }
    bool coverEdited() const { return m_coverChange != CoverChange::Keep || coverReferenceChanged(); }
    void undoCover();
    // Tell that an entry was changed through edit().
    void touched();

    EntryState state(int i) const;
    Summary summary() const;
    // Whether anything differs from what was opened (or the list is new and has tracks).
    bool isChanged() const;
    // Whether the captures are in another order than the file would give them (the ones it has by their tracks, then the ones added).
    bool orderChanged() const;
    // Whether a quick update is enough: only new metadata, added in the file itself with no copy (what Process does when it overwrites the file).
    bool canUpdate() const;
    // What is wrong with the shape of the capture in the header of its video track, when a shape was chosen (4:3 or 16:9): the header does not say what the track says, or the
    // metadata (isWidescreen) say the other shape. Empty when all agrees, or when the shape is not set (1:1: it is not worked out from the metadata). `fix` is the shape that
    // makes them agree.
    QString displayProblem(int i, QString *fix = nullptr) const;
    // Whether the tracks of samples of the capture are kept in another codec than the list says.
    bool needsRecode(int i) const;

    // What to run to write `output` from the list (as a stream when the list says so: the file is then written again with no index).
    Plan toPlan(const QString &output, bool verify) const;

signals:
    void changed();

private:
    bool companionsEdited(const Entry &e) const;

    QString m_archive;
    qint64 m_archiveSize = 0;
    bool m_hasIndex = true;
    bool m_stream = false;
    bool m_hasSession = false;
    QStringList m_sessionLines;
    SessionChange m_sessionChange = SessionChange::Keep;
    QString m_sessionFile;
    QByteArray m_cover;                 // the cover the file has (the attachment cover.png or cover.jpg)
    QByteArray m_coverNew;
    QByteArray m_none;
    QString m_coverNewSuffix;
    CoverChange m_coverChange = CoverChange::Keep;
    QString m_coverSource, m_coverSourceOriginal, m_coverLink, m_coverLinkOriginal;
    bool m_sessionEmbed = false;
    QJsonArray m_history;
    std::vector<Entry> m_entries;
    Codec m_codec = Codec::Raw;
    bool m_touched = false;
};

// The codec a track of a kind and depth is written in when the list says `codec` (what ld-container does: FLAC for 16-bit samples, FFV1 for CVBS).
Ldx::Codec codecFor(Codec codec, Ldx::Kind kind, int depth, Ldx::Codec current);
// Whether a track holds samples (the ones a codec is for).
bool isSamples(Ldx::Kind kind);

#endif // DOCUMENT_H
