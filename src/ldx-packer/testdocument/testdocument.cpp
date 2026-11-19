/******************************************************************************
 * testdocument.cpp
 * ldx-packer - unit tests for the list of captures and the commands that write it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "document.h"
#include "ldxcapture.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << __FILE__ << ":" << __LINE__ << ": check failed: " #condition "\n"; \
            std::exit(1); \
        } \
    } while (0)

// A file with two captures: 1 (raw, with its metadata 3, audio 4 and notes 6) and 2 (FLAC, with EFM 5)
static QString makeFile(const QString &directory)
{
    const QString path = directory + "/two.ldx";
    Ldx::Writer writer;
    Ldx::Track first;
    first.kind = Ldx::Kind::Tbc;
    first.codec = Ldx::Codec::Raw;
    first.name = "first.tbc";
    first.parameters["system"] = "PAL";
    writer.addTrack(first);
    Ldx::Track second = first;
    second.uid = 0;
    second.codec = Ldx::Codec::Flac;
    second.name = "second.tbc";
    writer.addTrack(second);
    Ldx::Track metadata;
    metadata.kind = Ldx::Kind::Metadata;
    metadata.depth = 0;
    metadata.parameters["track"] = 1;
    writer.addTrack(metadata);
    Ldx::Track audio;
    audio.kind = Ldx::Kind::Pcm;
    audio.parameters["track"] = 1;
    audio.parameters["sampleRate"] = 44100;
    writer.addTrack(audio);
    Ldx::Track efm;
    efm.kind = Ldx::Kind::Efm;
    efm.depth = 8;
    efm.parameters["track"] = 2;
    writer.addTrack(efm);
    Ldx::Track notes;
    notes.kind = Ldx::Kind::Annotations;
    notes.depth = 0;
    notes.parameters["track"] = 1;
    writer.addTrack(notes);

    QString error;
    CHECK(writer.open(path, error));
    CHECK(writer.writeBlock(first.number, 0, QByteArray(64, 'a')));
    CHECK(writer.writeBlock(notes.number, 0, QByteArray("a note")));
    CHECK(writer.writeBlock(first.number, 20000000, QByteArray(64, 'c')));
    CHECK(writer.finish(error));
    return path;
}

static QString argumentsOf(const Step &step)
{
    return step.arguments ? step.arguments().join(' ') : QString();
}

static void testOpen(const QString &path)
{
    std::cerr << "Testing opening a file\n";
    Document doc;
    QString error;
    CHECK(doc.open(path, &error));
    CHECK(doc.count() == 2);
    CHECK(doc.at(0).video.number == 1 && doc.at(1).video.number == 2);
    CHECK(doc.at(0).blocks == 2 && doc.at(1).blocks == 0);
    // the metadata and the audio go with the first, the EFM with the second, the notes are the notes of the first
    CHECK(doc.at(0).companions.size() == 2);
    CHECK(doc.at(1).companions.size() == 1 && doc.at(1).companions[0].kind == Ldx::Kind::Efm);
    CHECK(doc.at(0).notes == "a note" && doc.at(0).notesTrack == 6);
    CHECK(doc.codec() == Codec::AsIs);
    CHECK(!doc.isChanged());
    CHECK(doc.toPlan(path + ".out", false).steps.isEmpty());
    // a file that is not changed can still be written as a stream: the stream is written, then it takes its name
    doc.setStream(true);
    const Plan asStream = doc.toPlan(path + ".out", false);
    doc.setStream(false);
    CHECK(asStream.steps.size() == 2 && argumentsOf(asStream.steps[0]).startsWith("stream " + path + " -o ") && asStream.steps[1].action);

    Document notALdx;
    QFile plain(path + ".txt");
    CHECK(plain.open(QIODevice::WriteOnly));
    plain.write("not a container");
    plain.close();
    CHECK(!notALdx.open(path + ".txt", &error) && !error.isEmpty());
}

static void testRemoveAndRecode(const QString &path)
{
    std::cerr << "Testing the commands for a removal and a codec\n";
    Document doc;
    CHECK(doc.open(path, nullptr));
    doc.remove(1);
    CHECK(doc.state(1) == EntryState::Removed && doc.isChanged());
    Plan plan = doc.toPlan(path + ".out", false);
    // the video track and the EFM go; the file is then renamed
    CHECK(plan.steps.size() == 3);
    CHECK(argumentsOf(plan.steps[0]).startsWith("remove " + path + " --id 2 -o "));
    CHECK(argumentsOf(plan.steps[1]).contains("--id 5"));
    CHECK(plan.steps[2].action);

    doc.restore(1);
    CHECK(!doc.isChanged());
    // Auto keeps the TBC and the audio in FLAC: the first capture is raw, so it is the one to recode
    doc.setCodec(Codec::Auto);
    CHECK(doc.state(0) == EntryState::Recode && doc.state(1) == EntryState::Kept);
    plan = doc.toPlan(path + ".out", true);
    CHECK(plan.steps.size() == 3);
    CHECK(argumentsOf(plan.steps[0]).startsWith("recompress " + path + " --codec auto -o "));
    CHECK(argumentsOf(plan.steps[2]).startsWith("verify "));
    // Raw is for all the tracks: the second capture is the one that is FLAC
    doc.setCodec(Codec::Raw);
    CHECK(doc.state(0) == EntryState::Kept && doc.state(1) == EntryState::Recode);
}

static void testOrder(const QString &path)
{
    std::cerr << "Testing the order of the captures\n";
    Document doc;
    CHECK(doc.open(path, nullptr));
    CHECK(doc.canMove(0, 1) && !doc.canMove(0, -1) && !doc.orderChanged());
    doc.move(0, 1);
    CHECK(doc.at(0).video.number == 2 && doc.orderChanged() && doc.isChanged());
    Plan plan = doc.toPlan(path + ".out", false);
    // the file is written again with the tracks numbered to follow the list, and then it takes its name
    CHECK(plan.steps.size() == 2);
    CHECK(argumentsOf(plan.steps[0]).startsWith("reorder " + path + " --order 2,1 -o "));
    // moved back, it is as it was; a capture that is removed is not in the order
    doc.move(0, 1);
    CHECK(!doc.orderChanged() && !doc.isChanged());
    doc.move(0, 1);
    doc.remove(0);
    CHECK(!doc.orderChanged());
    plan = doc.toPlan(path + ".out", false);
    for (const Step &step : plan.steps) CHECK(!argumentsOf(step).startsWith("reorder"));
}

static void testMetadataAndSession(const QString &path, const QString &directory)
{
    std::cerr << "Testing the commands for the metadata and the session\n";
    Document doc;
    CHECK(doc.open(path, nullptr));
    CHECK(!doc.hasSession() && doc.hasIndex() && !doc.stream());

    // new metadata for the second capture: a block of its own, the current metadata from then on
    QJsonObject root;
    root["videoParameters"] = QJsonObject { { "system", "PAL" } };
    root["fields"] = QJsonArray();
    doc.setMetadata(1, root);
    CHECK(doc.state(1) == EntryState::Edited);
    QJsonObject back;
    CHECK(doc.metadataOf(1, &back, nullptr) && back.value("videoParameters").toObject().value("system").toString() == "PAL");
    Plan plan = doc.toPlan(path + ".out", false);
    CHECK(plan.steps.size() == 2);
    CHECK(argumentsOf(plan.steps[0]).startsWith("metadata " + path + " --id 2 --input ") && argumentsOf(plan.steps[0]).contains(".json -o "));
    doc.clearMetadata(1);
    CHECK(!doc.isChanged());

    // the shape the capture is shown in by the players: a step that writes it in the header and isWidescreen in the metadata
    doc.edit(1).aspect = "16:9";
    doc.touched();
    CHECK(doc.state(1) == EntryState::Edited);
    plan = doc.toPlan(path + ".out", false);
    CHECK(plan.steps.size() == 2 && argumentsOf(plan.steps[0]).startsWith("display " + path + " --id 2 --aspect 16:9 -o "));
    doc.edit(1).aspect.clear();
    doc.touched();
    CHECK(!doc.isChanged());

    // only new metadata, over the file that was opened: added in the file itself, no copy and no rename
    doc.setMetadata(1, root);
    CHECK(doc.canUpdate());
    plan = doc.toPlan(path, true);
    CHECK(plan.steps.size() == 2);
    CHECK(argumentsOf(plan.steps[0]).startsWith("metadata " + path + " --id 2 --input ") && !argumentsOf(plan.steps[0]).contains(" -o "));
    CHECK(argumentsOf(plan.steps[1]) == "verify " + path);
    // the shape, the name and notes where there were none are taken in the file too, in their place; metadata first, then the shape (which says
    // isWidescreen in the metadata that are then the current ones), the name, the notes
    doc.edit(1).aspect = "4:3";
    doc.edit(1).name = "second one";
    doc.edit(1).notes = "a note";
    doc.touched();
    CHECK(doc.canUpdate());
    plan = doc.toPlan(path, false);
    CHECK(plan.steps.size() == 4 && !plan.steps.back().action);
    CHECK(argumentsOf(plan.steps[1]) == "display " + path + " --id 2 --aspect 4:3");
    CHECK(argumentsOf(plan.steps[2]) == "rename " + path + " --id 2 --name second one");
    CHECK(argumentsOf(plan.steps[3]).startsWith("addtrack " + path + " --kind annotations --input ") && argumentsOf(plan.steps[3]).endsWith("--update"));
    // notes that change the ones the file has, or anything else with it: the file is written again (a copy of it is named in its own way)
    doc.edit(0).notes = "changed";
    doc.touched();
    CHECK(!doc.canUpdate());
    plan = doc.toPlan(path, false);
    CHECK(plan.steps.size() > 3 && plan.steps.back().action);
    bool renamed = false;
    for (const Step &step : plan.steps) renamed = renamed || (argumentsOf(step).startsWith("rename ") && argumentsOf(step).contains(" --id 2 --name second one -o "));
    CHECK(renamed);
    doc.edit(0).notes = doc.at(0).originalNotes;
    doc.edit(1).aspect.clear();
    doc.edit(1).name = doc.at(1).video.name;
    doc.edit(1).notes.clear();
    doc.clearMetadata(1);

    // the name and the parameters of the tracks: the sample rate of the audio, the system of the video, in the file's own list of tracks
    doc.edit(0).videoParameters["system"] = "NTSC";
    Companion &audio = doc.edit(0).companions[1];
    CHECK(audio.kind == Ldx::Kind::Pcm && audio.parameters.value("sampleRate").toDouble() == 44100.0);
    audio.parameters["sampleRate"] = 48000.0;
    audio.name = "audio at 48k";
    doc.touched();
    CHECK(doc.state(0) == EntryState::Edited && doc.canUpdate());
    plan = doc.toPlan(path, false);
    CHECK(plan.steps.size() == 2 && !plan.steps.back().action);
    CHECK(argumentsOf(plan.steps[0]) == "settrack " + path + " --id 1 --set system=\"NTSC\"");
    CHECK(argumentsOf(plan.steps[1]) == "settrack " + path + " --id 4 --name audio at 48k --set sampleRate=48000");
    // FLAC audio is coded at its rate: the file is written again, with the rate (an output) for the track
    audio.codec = Ldx::Codec::Flac;
    CHECK(!doc.canUpdate());
    plan = doc.toPlan(path + ".out", false);
    bool coded = false;
    for (const Step &step : plan.steps) coded = coded || (argumentsOf(step).startsWith("settrack ") && argumentsOf(step).contains(" --id 4 ") && argumentsOf(step).contains(" -o "));
    CHECK(coded);
    audio.codec = Ldx::Codec::Raw;
    audio.parameters["sampleRate"] = 44100.0;
    audio.name = audio.originalName;
    doc.edit(0).videoParameters["system"] = doc.at(0).video.parameters.value("system");
    doc.touched();
    CHECK(!doc.isChanged());

    // an audio track whose header declares another rate than its parameters (a file written before the header said the real rate): the sound is played at
    // the rate of the header; declaring the rate again is a step that sets it, even though the parameter does not change
    Companion &declared = doc.edit(0).companions[1];
    declared.headerRate = 48000.0;
    CHECK(declared.rateDiffers() && !doc.canUpdate());
    declared.declareRate = true;
    doc.touched();
    CHECK(doc.canUpdate());
    plan = doc.toPlan(path, false);
    CHECK(plan.steps.size() == 1 && argumentsOf(plan.steps[0]) == "settrack " + path + " --id 4 --set sampleRate=44100");
    declared.codec = Ldx::Codec::Flac;
    CHECK(!doc.canUpdate());
    declared.codec = Ldx::Codec::Raw;
    declared.declareRate = false;
    declared.headerRate = 0.0;
    doc.touched();
    CHECK(!doc.isChanged());

    // the shape in the header: it has to say what the track says (4:3 or 16:9), and the metadata; not set (1:1) is never worked out from the metadata
    Entry &shaped = doc.edit(0);
    const QJsonObject savedParameters = shaped.video.parameters;
    CHECK(doc.displayProblem(0).isEmpty());                         // (no shape chosen: nothing to say)
    shaped.metadataWide = 1;
    CHECK(doc.displayProblem(0).isEmpty());                         // (the metadata say widescreen, the shape is 1:1: it stays so)
    shaped.originalAspect = shaped.aspect = "4:3";
    shaped.video.headerDisplayWidth = 760;
    shaped.video.headerDisplayHeight = 525;
    QString fixTo;
    CHECK(!doc.displayProblem(0, &fixTo).isEmpty() && fixTo == "16:9");        // (the header says no shape of the track, and the metadata say 16:9)
    shaped.metadataWide = 0;
    shaped.video.parameters["system"] = "NTSC";
    shaped.video.parameters["fieldWidth"] = 910;
    shaped.video.parameters["fieldHeight"] = 263;
    shaped.video.headerPixelWidth = 910;
    shaped.video.headerPixelHeight = 263;
    shaped.videoParameters = shaped.video.parameters;
    CHECK(doc.displayProblem(0, &fixTo).isEmpty());                 // (760 x 525 is what 4:3 is for NTSC, and the metadata say 4:3)
    shaped.metadataWide = 1;
    CHECK(!doc.displayProblem(0, &fixTo).isEmpty() && fixTo == "16:9");
    shaped.aspect = "16:9";
    shaped.forceDisplay = true;
    doc.touched();
    CHECK(doc.canUpdate() && doc.displayProblem(0).isEmpty());
    plan = doc.toPlan(path, false);
    CHECK(plan.steps.size() == 1 && argumentsOf(plan.steps[0]) == "display " + path + " --id 1 --aspect 16:9");
    shaped.originalAspect.clear();
    shaped.aspect.clear();
    shaped.forceDisplay = false;
    shaped.metadataWide = -1;
    shaped.video.headerDisplayWidth = shaped.video.headerDisplayHeight = 0;
    shaped.video.parameters = shaped.videoParameters = savedParameters;
    doc.touched();
    CHECK(!doc.isChanged());

    // any track that goes with a capture can be taken out, the metadata too: a step that removes its id (and a track that is taken out is given back)
    doc.removeCompanion(1, 0);
    CHECK(doc.at(1).companions[0].removed && doc.state(1) == EntryState::Edited && !doc.canUpdate());
    plan = doc.toPlan(path + ".out", false);
    CHECK(plan.steps.size() == 2 && argumentsOf(plan.steps[0]).startsWith("remove " + path + " --id 5 -o "));
    doc.removeCompanion(1, 0);
    CHECK(!doc.at(1).companions[0].removed && !doc.isChanged());
    doc.removeCompanion(0, 0);
    CHECK(doc.at(0).companions[0].kind == Ldx::Kind::Metadata && doc.at(0).companions[0].removed);
    plan = doc.toPlan(path + ".out", false);
    CHECK(argumentsOf(plan.steps[0]).startsWith("remove " + path + " --id 3 -o "));
    doc.removeCompanion(0, 0);
    CHECK(!doc.isChanged());

    // a track added from a file, of a kind that is chosen, with its name and its rate
    QString addError;
    QFile text(directory + "/words.txt");
    CHECK(text.open(QIODevice::WriteOnly));
    text.write("words");
    text.close();
    CHECK(doc.addTrack(1, text.fileName(), Ldx::Kind::Annotations, "my words", 0.0, &addError));
    QFile samples(directory + "/a.pcm");
    CHECK(samples.open(QIODevice::WriteOnly));
    samples.write("1234");
    samples.close();
    CHECK(doc.addTrack(1, samples.fileName(), Ldx::Kind::Pcm, "a second audio", 32000.0, &addError));
    CHECK(!doc.addTrack(1, directory + "/absent.pcm", Ldx::Kind::Pcm, "x", 0.0, &addError) && !addError.isEmpty());
    CHECK(!doc.addTrack(1, text.fileName(), Ldx::Kind::Tbc, "x", 0.0, &addError));
    plan = doc.toPlan(path + ".out", false);
    int added = 0;
    for (const Step &step : plan.steps) {
        const QString a = argumentsOf(step);
        if (a.startsWith("addtrack ") && a.contains("--kind annotations") && a.contains("--name my words")) ++added;
        if (a.startsWith("addtrack ") && a.contains("--kind pcm") && a.contains("--name a second audio") && a.contains("--rate 32000")) ++added;
    }
    CHECK(added == 2);
    doc.removeCompanion(1, 2);
    doc.removeCompanion(1, 1);
    CHECK(!doc.isChanged());

    // a session is put in the file, or one that is there is taken out; a file that is not a session is refused
    QFile stack(directory + "/s.stack");
    CHECK(stack.open(QIODevice::WriteOnly));
    stack.write("{\"sources\": [{\"id\": \"a\", \"track\": 1}]}");
    stack.close();
    QString error;
    CHECK(doc.setSession(stack.fileName(), false, &error) && doc.sessionChange() == SessionChange::Set && doc.isChanged());
    plan = doc.toPlan(path + ".out", false);
    CHECK(plan.steps.size() == 2 && argumentsOf(plan.steps[0]).startsWith("stack " + path + " " + stack.fileName() + " -o ") && !argumentsOf(plan.steps[0]).contains("--embed"));
    CHECK(doc.setSession(stack.fileName(), true, &error));
    CHECK(argumentsOf(doc.toPlan(path + ".out", false).steps[0]).contains("--embed --codec"));
    doc.keepSession();
    CHECK(!doc.isChanged());
    QFile plain(directory + "/plain.stack");
    CHECK(plain.open(QIODevice::WriteOnly));
    plain.write("{}");
    plain.close();
    CHECK(!doc.setSession(plain.fileName(), false, &error) && !error.isEmpty() && doc.sessionChange() == SessionChange::Keep);

    // the layout: a file with its index written as a stream, and the other way round
    doc.setStream(true);
    plan = doc.toPlan(path + ".out", false);
    CHECK(plan.steps.size() == 2 && argumentsOf(plan.steps[0]).startsWith("stream "));
    doc.setStream(false);
    CHECK(!doc.isChanged());
}

static void testNotesAndFiles(const QString &path, const QString &directory)
{
    std::cerr << "Testing the commands for notes and files\n";
    Document doc;
    CHECK(doc.open(path, nullptr));
    doc.edit(0).notes = "changed";
    doc.touched();
    CHECK(doc.state(0) == EntryState::Edited);
    QFile efm(directory + "/more.efm");
    CHECK(efm.open(QIODevice::WriteOnly));
    efm.write("t-values");
    efm.close();
    QString error;
    CHECK(doc.addCompanion(1, efm.fileName(), &error));
    CHECK(!doc.addCompanion(1, directory + "/more.txt", &error));
    const Plan plan = doc.toPlan(path + ".out", false);
    // the old notes go, the new ones are added to the first capture (1), then the EFM to the second (2)
    CHECK(plan.steps.size() == 4);
    CHECK(argumentsOf(plan.steps[0]).contains("--id 6"));
    CHECK(argumentsOf(plan.steps[1]).contains("--kind annotations") && argumentsOf(plan.steps[1]).contains("--about 1"));
    CHECK(argumentsOf(plan.steps[2]).contains("--kind efm") && argumentsOf(plan.steps[2]).contains("--about 2"));

    // a .wav file is read into raw audio first, at its rate; the metadata cannot be added
    QFile wav(directory + "/more.wav");
    CHECK(wav.open(QIODevice::WriteOnly));
    QByteArray header("RIFF\x30\x00\x00\x00WAVEfmt \x10\x00\x00\x00\x01\x00\x02\x00\x80\xbb\x00\x00\x00\xee\x02\x00\x04\x00\x10\x00" "data\x0c\x00\x00\x00", 44);
    wav.write(header + QByteArray(12, 'x'));
    wav.close();
    CHECK(doc.addCompanion(0, wav.fileName(), &error));
    CHECK(doc.at(0).companions.back().sampleRate == 48000.0);
    CHECK(!doc.addCompanion(0, directory + "/x.tbc.db", &error) && error.contains("metadata"));
    const Plan withWav = doc.toPlan(path + ".out", false);
    int reads = 0;
    for (const Step &step : withWav.steps) {
        if (step.action && step.description.contains("audio")) {
            QString why;
            CHECK(step.action(&why));
            ++reads;
        }
    }
    CHECK(reads == 1);
    // the audio that is added is FLAC when the list keeps its samples in FLAC
    for (const Step &step : withWav.steps) CHECK(!argumentsOf(step).contains("--codec flac"));
    doc.setCodec(Codec::Flac);
    int flacAudio = 0;
    for (const Step &step : doc.toPlan(path + ".out", false).steps) {
        if (argumentsOf(step).contains("--kind pcm") && argumentsOf(step).contains("--codec flac")) ++flacAudio;
    }
    CHECK(flacAudio == 1);
    QFile raw(withWav.directory->filePath("audio1.pcm"));
    CHECK(raw.open(QIODevice::ReadOnly) && raw.readAll() == QByteArray(12, 'x'));
}

// A Y/C capture: its chroma is a line of the tracks that go with the luma, taken out alone, taken out with the luma, and put back or added
static void testChroma(const QString &directory)
{
    std::cerr << "Testing a Y/C capture\n";
    const QString path = directory + "/yc.ldx";
    {
        Ldx::Writer writer;
        Ldx::Track luma;
        luma.kind = Ldx::Kind::Tbc;
        luma.codec = Ldx::Codec::Raw;
        luma.name = "pair.tbc";
        luma.parameters["system"] = "PAL";
        writer.addTrack(luma);
        Ldx::Track chroma = luma;
        chroma.uid = 0;
        chroma.name = "pair.tbc (chroma)";
        chroma.parameters["role"] = "chroma";
        chroma.parameters["track"] = 1;
        writer.addTrack(chroma);
        QString error;
        CHECK(writer.open(path, error));
        CHECK(writer.writeBlock(luma.number, 0, QByteArray(64, 'l')));
        CHECK(writer.writeBlock(chroma.number, 0, QByteArray(64, 'c')));
        CHECK(writer.finish(error));
    }
    Document doc;
    QString error;
    CHECK(doc.open(path, &error));
    // one capture (the luma); the chroma is a line of what goes with it
    CHECK(doc.count() == 1 && doc.at(0).companions.size() == 1 && doc.at(0).companions[0].chroma && doc.at(0).companions[0].number == 2);
    CHECK(doc.at(0).chroma.number == 2);
    // a capture has one chroma
    QFile chromaFile(directory + "/pair_chroma.tbc");
    CHECK(chromaFile.open(QIODevice::WriteOnly));
    chromaFile.write("x");
    chromaFile.close();
    CHECK(!doc.addChroma(0, chromaFile.fileName(), QString(), &error) && !error.isEmpty());
    // taken out alone: the luma stays
    doc.removeCompanion(0, 0);
    Plan plan = doc.toPlan(path + ".out", false);
    CHECK(plan.steps.size() == 2 && argumentsOf(plan.steps[0]).startsWith("remove " + path + " --id 2 -o "));
    // and another is added (a file of the kind of the luma)
    CHECK(!doc.addChroma(0, directory + "/pair.cvbsc", QString(), &error));
    CHECK(doc.addChroma(0, chromaFile.fileName(), QString(), &error));
    plan = doc.toPlan(path + ".out", false);
    bool added = false;
    for (const Step &step : plan.steps) added = added || (argumentsOf(step).startsWith("addchroma ") && argumentsOf(step).contains(" --about 1 --input ") && argumentsOf(step).contains(" --codec raw -o "));
    CHECK(added && !doc.canUpdate());
    doc.removeCompanion(0, 1);
    doc.removeCompanion(0, 0);
    CHECK(!doc.isChanged());
    // taken out with its luma: ld-container remove does both, so there is one step
    doc.remove(0);
    plan = doc.toPlan(path + ".out", false);
    CHECK(plan.steps.size() == 2 && argumentsOf(plan.steps[0]).startsWith("remove " + path + " --id 1 -o "));
}


// The captures of another .ldx file are put in the list (not opened) and come in with their tracks, in the file that is written
static void testMerge(const QString &path, const QString &directory)
{
    std::cerr << "Testing the captures of another .ldx file\n";
    Document doc;
    QString error;
    // nothing to put them in
    CHECK(!doc.addArchive(path, &error) && !error.isEmpty());
    CHECK(doc.open(path, nullptr));
    const QString other = directory + "/yc.ldx";
    CHECK(!doc.addArchive(path, &error));                           // (the file that is opened)
    CHECK(doc.addArchive(other, &error));
    CHECK(doc.count() == 3 && doc.at(2).ldxFile == other && doc.at(2).ldxTrack == 1 && !doc.at(2).fromArchive);
    CHECK(doc.state(2) == EntryState::New && doc.isChanged() && !doc.canUpdate());
    // its chroma is one of its tracks: taken out, it is left out of what is put in
    CHECK(doc.at(2).companions.size() == 1 && doc.at(2).companions[0].chroma);
    doc.edit(2).name = "from the other file";
    doc.removeCompanion(2, 0);
    Plan plan = doc.toPlan(path + ".out", false);
    CHECK(plan.steps.size() == 2);
    CHECK(argumentsOf(plan.steps[0]).startsWith("merge " + path + " " + other + " --id 1 --exclude 2 --name from the other file --codec asis -o "));
    CHECK(plan.steps[1].action);
    // the order: the new ones come after the ones of the file, whatever they are
    CHECK(!doc.orderChanged());
    doc.move(2, -1);
    CHECK(doc.orderChanged());
    doc.move(1, 1);
    CHECK(!doc.orderChanged());
    doc.remove(2);
    CHECK(doc.count() == 2 && !doc.isChanged());
}


// The cover of the file: put in, changed or taken out, a step that writes the file again
static void testCover(const QString &path)
{
    std::cerr << "Testing the cover\n";
    Document doc;
    CHECK(doc.open(path, nullptr));
    CHECK(!doc.hasCover() && !doc.fileHasCover() && !doc.isChanged());
    doc.setCover(QByteArray("\x89PNG-data"), "png");
    CHECK(doc.hasCover() && doc.coverChange() == CoverChange::Set && doc.isChanged() && doc.canUpdate());
    Plan plan = doc.toPlan(path + ".out", false);
    CHECK(plan.steps.size() == 2 && argumentsOf(plan.steps[0]).startsWith("cover " + path + " --set ") && argumentsOf(plan.steps[0]).contains(".png -o "));
    // over the file that was opened it is added in the file itself, at its end: no copy and no new file
    plan = doc.toPlan(path, false);
    CHECK(plan.steps.size() == 1 && argumentsOf(plan.steps[0]).startsWith("cover " + path + " --set ") && !argumentsOf(plan.steps[0]).contains(" -o ") && !plan.steps[0].action);
    doc.keepCover();
    CHECK(!doc.hasCover() && !doc.isChanged());
    // taking out a cover the file does not have is no step
    doc.removeCover();
    CHECK(doc.coverChange() == CoverChange::Keep && !doc.isChanged());
    doc.keepCover();
    // an address for the picture, and a link: options of the same step, in place or by a copy
    doc.setCoverSource("pictures/disc.jpg");
    doc.setCoverLink("https://example.org/disc");
    CHECK(doc.coverSource() == "pictures/disc.jpg" && doc.coverReferenceChanged() && doc.coverEdited() && doc.isChanged() && doc.canUpdate());
    plan = doc.toPlan(path, false);
    CHECK(plan.steps.size() == 1 && argumentsOf(plan.steps[0]) == "cover " + path + " --source pictures/disc.jpg --link https://example.org/disc");
    plan = doc.toPlan(path + ".out", false);
    CHECK(plan.steps.size() == 2 && argumentsOf(plan.steps[0]).startsWith("cover " + path + " --source pictures/disc.jpg --link ") && argumentsOf(plan.steps[0]).contains(" -o "));
    // taking them back out: nothing was there, so there is nothing to do
    doc.setCoverSource(QString());
    doc.setCoverLink(QString());
    CHECK(!doc.coverEdited() && !doc.isChanged());
    doc.setCoverLink("https://example.org/a");
    doc.undoCover();
    CHECK(doc.coverLink().isEmpty() && !doc.isChanged());
    CHECK(!doc.isChanged());
}

// What a person says of a capture (the medium, the reference, the copy, the capture, the face: parameters of the video track; the colour standard: a member of the
// metadata), and where the capture comes from (the track ldx.rfsource): the first are steps that change a parameter or tag the capture, the track is a companion that
// is not taken for the metadata of the capture
static void testSaidAndSource(const QString &directory)
{
    std::cerr << "Testing what a person says of a capture, and where it comes from\n";
    const QString path = directory + "/said.ldx";
    QFile::copy(directory + "/two.ldx", path);
    QString error;
    const QJsonObject rf{ { "name", "cap.s16" }, { "rate_hz", 75000000.0 }, { "bits", 10 } };
    CHECK(LdxCapture::putRfSourceInPlace(path, 1, QJsonDocument(rf).toJson(QJsonDocument::Compact), LdxCapture::historyEntry("rfsource", "put in"), error));
    QByteArray json;
    CHECK(LdxCapture::readRfSource(path, 1, json, error) && QJsonDocument::fromJson(json).object().value("name").toString() == "cap.s16");
    CHECK(!LdxCapture::readRfSource(path, 2, json, error));          // (the other capture says nothing)

    Document doc;
    CHECK(doc.open(path, nullptr));
    CHECK(!doc.isChanged());
    bool seen = false;
    for (const Companion &c : doc.at(0).companions) seen = seen || (c.kind == Ldx::Kind::Metadata && c.parameters.value("namespace").toString() == "ldx.rfsource");
    CHECK(seen);

    // The parameters of the video track
    doc.edit(0).videoParameters["face"] = "A";
    doc.edit(0).videoParameters["copy"] = 2;
    doc.touched();
    CHECK(doc.isChanged() && doc.canUpdate());
    Plan plan = doc.toPlan(path, false);
    CHECK(plan.steps.size() == 1 && argumentsOf(plan.steps[0]).startsWith("settrack " + path + " --id 1 --set ") && argumentsOf(plan.steps[0]).contains("face=\"A\"")
          && argumentsOf(plan.steps[0]).contains("copy=2"));
    doc.edit(0).videoParameters = doc.at(0).video.parameters;
    doc.touched();
    CHECK(!doc.isChanged());

    // The system can be SECAM or NTSC-VHD: the video track says it with a step of settrack (which keeps the metadata of the capture in agreement), in the file itself or on a copy
    doc.edit(0).videoParameters["system"] = "SECAM";
    doc.touched();
    CHECK(doc.isChanged() && doc.canUpdate());
    plan = doc.toPlan(path, false);
    CHECK(plan.steps.size() == 1 && argumentsOf(plan.steps[0]) == "settrack " + path + " --id 1 --set system=\"SECAM\"" && !plan.steps[0].action);
    plan = doc.toPlan(path + ".out", false);
    bool said = false;
    for (const Step &step : plan.steps) said = said || (argumentsOf(step).startsWith("settrack ") && argumentsOf(step).contains(" --id 1 --set system=\"SECAM\" -o "));
    CHECK(said);
    doc.edit(0).videoParameters = doc.at(0).video.parameters;
    doc.touched();
    CHECK(!doc.isChanged());
    // Where it comes from: what the file has is read, changed with a step of ld-container rfsource, or taken out
    CHECK(!doc.at(0).originalRfSource.isEmpty() && doc.at(0).rfSource == doc.at(0).originalRfSource);
    doc.edit(0).rfSource = QJsonDocument(QJsonObject{ { "name", "other.s16" }, { "bits", 12 } }).toJson(QJsonDocument::Compact);
    doc.touched();
    CHECK(doc.isChanged() && doc.canUpdate());
    plan = doc.toPlan(path, false);
    CHECK(plan.steps.size() == 1 && argumentsOf(plan.steps[0]).startsWith("rfsource " + path + " --id 1 --set ") && !argumentsOf(plan.steps[0]).contains(" -o "));
    doc.edit(0).rfSource.clear();
    doc.touched();
    plan = doc.toPlan(path, false);
    CHECK(plan.steps.size() == 1 && argumentsOf(plan.steps[0]) == "rfsource " + path + " --id 1 --remove");
    plan = doc.toPlan(path + ".out", false);
    bool removed = false;
    for (const Step &step : plan.steps) removed = removed || argumentsOf(step).startsWith("rfsource ") && argumentsOf(step).contains(" --remove -o ");
    CHECK(removed);
    doc.edit(0).rfSource = doc.at(0).originalRfSource;
    doc.touched();
    CHECK(!doc.isChanged());
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString path = makeFile(directory.path());
    testOpen(path);
    testRemoveAndRecode(path);
    testOrder(path);
    testMetadataAndSession(path, directory.path());
    testNotesAndFiles(path, directory.path());
    testChroma(directory.path());
    testMerge(path, directory.path());
    testCover(path);
    testSaidAndSource(directory.path());
    std::cerr << "All the tests passed\n";
    return 0;
}
