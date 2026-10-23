/******************************************************************************
 * testmetadataoptions.cpp
 * ld-decode-tools TBC library - unit test of the command-line options that choose the metadata file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

#include "cvbs.h"
#include "metadataoptions.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            std::fflush(stderr); \
            std::exit(1); \
        } \
    } while (0)

using MetadataFormat = LdDecodeMetaData::MetadataFormat;

static void touch(const QString &name, const QByteArray &content)
{
    QFile file(name);
    CHECK(file.open(QIODevice::WriteOnly));
    file.write(content);
}

static void makeSqlite(const QString &name) { touch(name, QByteArray("SQLite format 3\0", 16) + QByteArray(100, '\0')); }
static void makeJson(const QString &name) { touch(name, "{\"videoParameters\":{}}"); }

// Options as a tool is given them, parsed and processed
static bool parse(MetadataOptions &options, const QStringList &arguments)
{
    QCommandLineParser parser;
    options.addTo(parser);
    if (!parser.parse(QStringList() << "tool" << arguments)) return false;
    return options.process(parser);
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    CHECK(directory.isValid());

    const int both = MetadataOptions::InputFile | MetadataOptions::OutputFile;

    // --meta and the file options, under their names and the original ones
    {
        MetadataOptions none(both);
        CHECK(parse(none, {}) && !none.isFormatSet() && !none.isInputFileSet() && !none.isOutputFileSet());

        MetadataOptions json(both);
        CHECK(parse(json, { "--meta", "json" }) && json.isFormatSet() && json.format() == MetadataFormat::Json);
        MetadataOptions db(both);
        CHECK(parse(db, { "--meta", "DB" }) && db.format() == MetadataFormat::Sqlite);
        MetadataOptions sqlite(both);
        CHECK(parse(sqlite, { "--metadata-format", "sqlite" }) && sqlite.format() == MetadataFormat::Sqlite);
        MetadataOptions bad(both);
        CHECK(!parse(bad, { "--meta", "xml" }));

        MetadataOptions files(both);
        CHECK(parse(files, { "--input-metadata", "in.db", "--output-metadata", "out.db" }) && files.isInputFileSet() && files.isOutputFileSet());
        MetadataOptions aliases(both);
        CHECK(parse(aliases, { "--input-json", "in.json", "--output-json", "out.json" }) && aliases.isInputFileSet() && aliases.isOutputFileSet());
        MetadataOptions twice(both);
        CHECK(!parse(twice, { "--input-metadata", "a.db", "--input-json", "b.db" }));
        MetadataOptions twiceOut(both);
        CHECK(!parse(twiceOut, { "--output-metadata", "a.db", "--output-json", "b.db" }));

        // A tool that has no file options does not take them
        MetadataOptions formatOnly(MetadataOptions::FormatOnly);
        CHECK(!parse(formatOnly, { "--input-metadata", "a.db" }));
    }

    // The metadata file to read for a capture
    {
        const QString tbc = directory.filePath("cap.tbc");
        const QString sqliteFile = LdDecodeMetaData::metadataFileName(tbc, MetadataFormat::Sqlite);
        const QString jsonFile = LdDecodeMetaData::metadataFileName(tbc, MetadataFormat::Json);
        QString selected;

        // Neither: the default is named, so that the read says it is not there
        MetadataOptions plain(both);
        CHECK(parse(plain, {}));
        CHECK(plain.selectInput(tbc, true, selected) && selected == sqliteFile);

        // Only one: that one
        makeJson(jsonFile);
        CHECK(plain.selectInput(tbc, true, selected) && selected == jsonFile);
        QFile::remove(jsonFile);
        makeSqlite(sqliteFile);
        CHECK(plain.selectInput(tbc, true, selected) && selected == sqliteFile);

        // Both: SQLite, unless --meta says JSON
        makeJson(jsonFile);
        CHECK(plain.selectInput(tbc, true, selected) && selected == sqliteFile);
        MetadataOptions wantJson(both);
        CHECK(parse(wantJson, { "--meta", "json" }));
        CHECK(wantJson.selectInput(tbc, true, selected) && selected == jsonFile);

        // The format asked for is not there
        QFile::remove(sqliteFile);
        MetadataOptions wantDb(both);
        CHECK(parse(wantDb, { "--meta", "db" }));
        CHECK(!wantDb.selectInput(tbc, true, selected));
        QFile::remove(jsonFile);

        // A file named: used as it is, if it is there and agrees with --meta
        const QString named = directory.filePath("named.json");
        MetadataOptions byName(both);
        CHECK(parse(byName, { "--input-metadata", named }));
        CHECK(!byName.selectInput(tbc, true, selected));           // (not there)
        makeJson(named);
        CHECK(byName.selectInput(tbc, true, selected) && selected == named);
        CHECK(byName.selectInput(tbc, false, selected) && selected == sqliteFile);       // (a tool that does not use the option)
        MetadataOptions disagree(both);
        CHECK(parse(disagree, { "--input-metadata", named, "--meta", "db" }));
        CHECK(!disagree.selectInput(tbc, true, selected));
        MetadataOptions agree(both);
        CHECK(parse(agree, { "--input-metadata", named, "--meta", "json" }));
        CHECK(agree.selectInput(tbc, true, selected) && selected == named);

        // The container formats hold their metadata in themselves
        CHECK(plain.selectInput(directory.filePath("a.ldx"), true, selected) && selected == directory.filePath("a.ldx"));
        CHECK(plain.selectInput(directory.filePath("a.ldx#2"), true, selected) && selected == directory.filePath("a.ldx#2"));
        CHECK(plain.selectInput(directory.filePath("s.stack"), true, selected) && selected == directory.filePath("s.stack"));

        // A CVBS capture with only its core metadata
        const QString cvbs = directory.filePath("c.cvbs");
        touch(cvbs, "x");
        touch(Cvbs::metaFileName(cvbs), "{}");
        CHECK(plain.selectInput(cvbs, true, selected) && selected == Cvbs::metaFileName(cvbs));
    }

    // The metadata file to write: in the format of the input, never a conversion
    {
        QString selected;
        MetadataOptions plain(both);
        CHECK(parse(plain, {}));
        CHECK(plain.selectOutput(directory.filePath("o.tbc.db"), MetadataFormat::Sqlite, selected) && selected == directory.filePath("o.tbc.db"));
        CHECK(!plain.selectOutput(directory.filePath("o.tbc.json"), MetadataFormat::Sqlite, selected));
        CHECK(!plain.selectOutput(directory.filePath("o.tbc.db"), MetadataFormat::Json, selected));

        MetadataOptions named(both);
        CHECK(parse(named, { "--output-metadata", directory.filePath("x.json") }));
        CHECK(named.selectOutput(directory.filePath("o.tbc.db"), MetadataFormat::Json, selected) && selected == directory.filePath("x.json"));
        CHECK(!named.selectOutput(directory.filePath("o.tbc.db"), MetadataFormat::Sqlite, selected));

        // A new file with another extension is written in the format of the input; an existing file is known by its contents
        MetadataOptions other(both);
        CHECK(parse(other, { "--output-metadata", directory.filePath("meta.dat") }));
        CHECK(other.selectOutput("", MetadataFormat::Json, selected) && selected == directory.filePath("meta.dat"));
        makeSqlite(directory.filePath("meta.dat"));
        CHECK(!other.selectOutput("", MetadataFormat::Json, selected));
        CHECK(other.selectOutput("", MetadataFormat::Sqlite, selected));
    }

    CHECK(MetadataOptions::formatName(MetadataFormat::Json) == "JSON" && MetadataOptions::formatName(MetadataFormat::Sqlite) == "SQLite");

    std::printf("testmetadataoptions: ok\n");
    return 0;
}
