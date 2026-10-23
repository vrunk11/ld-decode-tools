/*******************************************************************************
 * standardoptions.hpp
 *
 * ld-process-ac3 - the options that every tool of ld-decode-tools has
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#pragma once

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "logger.hpp"

// The two programs read their options with getopt, which knows short options only, so the options of the other tools (--help, --version, -d --debug,
// -q --quiet) and the long names of their own (--log-level for -v, --sliding-average-window for -s) are taken from the command line first. -d and -q
// set the logging level (debug, or errors only) before getopt reads -v, so that an explicit -v wins.
//
// A whole number from a command line: all of the text is the number, and it is between minimum and maximum. (std::stoi throws on text that is not a number, which
// stops a program that has not caught it, and takes "12abc" for 12.)
inline bool parseWholeNumber(const char *text, long minimum, long maximum, int &value)
{
    if (text == nullptr || *text == '\0') return false;
    char *end = nullptr;
    errno = 0;
    const long number = std::strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || number < minimum || number > maximum) return false;
    value = static_cast<int>(number);
    return true;
}

// Returns -1 when the program goes on with the options that are left in argc and argv, or the exit code when it has done what was asked (--help, --version).
inline int takeStandardOptions(int &argc, char *argv[], const std::string &application, void (*help)(const std::string &))
{
    static char shortLogLevel[] = "-v";
    static char shortWindow[] = "-s";

    int kept = 1;
    for (int i = 1; i < argc; i++) {
        const char *argument = argv[i];
        if (std::strcmp(argument, "--help") == 0 || std::strcmp(argument, "-h") == 0) {
            help(argv[0]);
            return 0;
        }
        if (std::strcmp(argument, "--version") == 0) {
            std::cout << application << " ld-decode-tools - Branch: " << APP_BRANCH << " / Commit: " << APP_COMMIT << std::endl;
            return 0;
        }
        if (std::strcmp(argument, "-d") == 0 || std::strcmp(argument, "--debug") == 0) {
            Logger::GLOBAL_LOG_LEVEL = DEBU;
            continue;
        }
        if (std::strcmp(argument, "-q") == 0 || std::strcmp(argument, "--quiet") == 0) {
            Logger::GLOBAL_LOG_LEVEL = ERRR;
            continue;
        }
        if (std::strcmp(argument, "--log-level") == 0) argv[i] = shortLogLevel;
        if (std::strcmp(argument, "--sliding-average-window") == 0) argv[i] = shortWindow;
        argv[kept++] = argv[i];
    }
    argv[kept] = nullptr;
    argc = kept;
    return -1;
}
