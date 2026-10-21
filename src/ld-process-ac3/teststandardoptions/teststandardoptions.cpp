/*******************************************************************************
 * teststandardoptions.cpp
 * ld-process-ac3 - unit tests for takeStandardOptions
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include <cstdlib>
#include <iostream>
#include <vector>

#include "standardoptions.hpp"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " #condition " (line " << __LINE__ << ")\n"; \
            std::exit(1); \
        } \
    } while (0)

static int helpCalls = 0;
static void help(const std::string &) { helpCalls++; }

// Run takeStandardOptions on a command line and give back what is left of it
static int run(std::vector<std::string> words, std::vector<std::string> &left)
{
    std::vector<char *> argv;
    for (std::string &word: words) argv.push_back(word.data());
    argv.push_back(nullptr);
    int argc = static_cast<int>(words.size());
    const int result = takeStandardOptions(argc, argv.data(), "tool", help);
    left.clear();
    for (int i = 0; i < argc; i++) left.push_back(argv[i]);
    CHECK(argv[argc] == nullptr);
    return result;
}

int main()
{
    std::cerr << "Testing takeStandardOptions\n";
    std::vector<std::string> left;

    // Nothing to take: the command line is left as it is
    Logger::GLOBAL_LOG_LEVEL = INFO;
    CHECK(run({ "tool", "-v", "2", "in", "out" }, left) == -1);
    CHECK((left == std::vector<std::string>{ "tool", "-v", "2", "in", "out" }));
    CHECK(Logger::GLOBAL_LOG_LEVEL == INFO);

    // -d and -q set the level and are taken out; --debug and --quiet are the same
    CHECK(run({ "tool", "-d", "in", "out" }, left) == -1);
    CHECK((left == std::vector<std::string>{ "tool", "in", "out" }));
    CHECK(Logger::GLOBAL_LOG_LEVEL == DEBU);
    CHECK(run({ "tool", "in", "--quiet", "out" }, left) == -1);
    CHECK((left == std::vector<std::string>{ "tool", "in", "out" }));
    CHECK(Logger::GLOBAL_LOG_LEVEL == ERRR);
    CHECK(run({ "tool", "--debug" }, left) == -1);
    CHECK(Logger::GLOBAL_LOG_LEVEL == DEBU);
    CHECK(run({ "tool", "-q" }, left) == -1);
    CHECK(Logger::GLOBAL_LOG_LEVEL == ERRR);

    // The long names of -v and -s are given to getopt as the short ones
    CHECK(run({ "tool", "--log-level", "1", "--sliding-average-window", "500", "in", "out" }, left) == -1);
    CHECK((left == std::vector<std::string>{ "tool", "-v", "1", "-s", "500", "in", "out" }));

    // --help and --version do what they say and end the program, with the other words untouched
    helpCalls = 0;
    CHECK(run({ "tool", "--help" }, left) == 0);
    CHECK(run({ "tool", "-h" }, left) == 0);
    CHECK(helpCalls == 2);
    CHECK(run({ "tool", "--version" }, left) == 0);

    std::cerr << "All tests passed\n";
    return 0;
}
