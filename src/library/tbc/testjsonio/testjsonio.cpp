/******************************************************************************
 * testjsonio.cpp
 * ld-decode-tools TBC library - unit test of the JSON reader and writer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <QString>
#include <QVector>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "jsonio.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            std::fflush(stderr); \
            std::exit(1); \
        } \
    } while (0)

// Whether reading the text as a string fails
static bool stringFails(const char *text)
{
    std::istringstream input(text);
    JsonReader reader(input);
    try {
        std::string value;
        reader.read(value);
    } catch (JsonReader::Error &) {
        return true;
    }
    return false;
}

static std::string stringOf(const char *text)
{
    std::istringstream input(text);
    JsonReader reader(input);
    std::string value;
    reader.read(value);
    return value;
}

static bool discardFails(const char *text)
{
    std::istringstream input(text);
    JsonReader reader(input);
    try {
        reader.discard();
    } catch (JsonReader::Error &) {
        return true;
    }
    return false;
}

int main()
{
    // Written and read back: objects, arrays, numbers, booleans, strings that need escapes
    {
        std::ostringstream out;
        JsonWriter writer(out);
        writer.beginObject();
        writer.writeMember("count", 42);
        writer.writeMember("big", static_cast<qint64>(9007199254740991LL));
        writer.writeMember("ratio", 0.1);
        writer.writeMember("flag", true);
        writer.writeMember("name", "quote \" back \\ tab \t newline \n");
        writer.writeMember("text", QString::fromUtf8("caf\xc3\xa9 \xe2\x82\xac"));
        writer.writeMember("list");
        writer.beginArray();
        for (int i = 0; i < 3; i++) {
            writer.writeElement();
            writer.write(i * i);
        }
        writer.endArray();
        writer.writeMember("empty");
        writer.beginObject();
        writer.endObject();
        writer.endObject();

        std::istringstream input(out.str());
        JsonReader reader(input);
        int count = 0, ints = 0;
        qint64 big = 0;
        double ratio = 0;
        bool flag = false;
        std::string name;
        QString text;
        std::string member;
        QVector<int> list;
        reader.beginObject();
        while (reader.readMember(member)) {
            if (member == "count") reader.read(count);
            else if (member == "big") reader.read(big);
            else if (member == "ratio") reader.read(ratio);
            else if (member == "flag") reader.read(flag);
            else if (member == "name") reader.read(name);
            else if (member == "text") reader.read(text);
            else if (member == "list") {
                reader.beginArray();
                while (reader.readElement()) { reader.read(ints); list.append(ints); }
                reader.endArray();
            } else if (member == "empty") {
                reader.beginObject();
                CHECK(!reader.readMember(member));
                reader.endObject();
            } else reader.discard();
        }
        reader.endObject();
        CHECK(count == 42 && big == 9007199254740991LL && ratio == 0.1 && flag);
        CHECK(name == "quote \" back \\ tab \t newline \n");
        CHECK(text == QString::fromUtf8("caf\xc3\xa9 \xe2\x82\xac"));
        CHECK(list == QVector<int>({ 0, 1, 4 }));
    }

    // Numbers as JSON writes them: exponents, negative, an integer written as a real
    {
        std::istringstream input("[1e3, -0.5, 2.50E+1, 12.0, -7]");
        JsonReader reader(input);
        double a, b, c;
        int d, e;
        reader.beginArray();
        CHECK(reader.readElement()); reader.read(a);
        CHECK(reader.readElement()); reader.read(b);
        CHECK(reader.readElement()); reader.read(c);
        CHECK(reader.readElement()); reader.read(d);
        CHECK(reader.readElement()); reader.read(e);
        CHECK(!reader.readElement());
        reader.endArray();
        CHECK(a == 1000.0 && b == -0.5 && c == 25.0 && d == 12 && e == -7);
    }

    // Text that is not JSON is an error with the place, not a wrong value
    {
        std::istringstream input("{\"a\" 1}");
        JsonReader reader(input);
        std::string member;
        reader.beginObject();
        bool failed = false;
        try { reader.readMember(member); } catch (JsonReader::Error &error) { failed = std::string(error.what()).find("at byte") != std::string::npos; }
        CHECK(failed);
    }
    CHECK(discardFails("[1, 2"));
    CHECK(discardFails("{\"a\": }"));
    CHECK(discardFails("[1 2]"));
    CHECK(discardFails("-"));
    CHECK(discardFails("1.x"));
    CHECK(stringFails("\"never closed"));
    CHECK(stringFails("\"bad \\q escape\""));
    CHECK(stringFails("12"));

    // Values that are skipped, of any type, nested
    CHECK(!discardFails("{\"a\": [1, {\"b\": \"c\"}, true, false], \"d\": -1.5e2}"));

    // JSON from other programs: null (Python's None), and \u escapes (json.dumps writes every non-ASCII character that way)
    CHECK(!discardFails("{\"a\": null, \"b\": [null, 1]}"));
    CHECK(stringOf("\"caf\\u00e9\"") == "caf\xc3\xa9");
    CHECK(stringOf("\"\\u20ac \\u0041\"") == "\xe2\x82\xac A");
    CHECK(stringOf("\"\\ud83d\\ude00\"") == "\xf0\x9f\x98\x80");            // (a character outside the first plane, as two surrogates)
    CHECK(stringFails("\"\\u12\""));
    CHECK(stringFails("\"\\u12g4\""));

    // Control characters are written escaped, so that what is written is JSON
    {
        std::ostringstream out;
        JsonWriter writer(out);
        writer.write("a\x01" "b\x1f" "c");
        CHECK(out.str() == "\"a\\u0001b\\u001fc\"");
        CHECK(stringOf(out.str().c_str()) == "a\x01" "b\x1f" "c");
    }

    std::printf("testjsonio: ok\n");
    return 0;
}
