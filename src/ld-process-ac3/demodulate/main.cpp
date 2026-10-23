/*******************************************************************************
 * main.cpp
 *
 * ld-process-ac3 - AC3-RF decoder
 * Copyright (C) 2022-2022 Leighton Smallshire & Ian Smallshire
 *
 * Derived from prior work by Staffan Ulfberg with feedback
 * to original author. (Copyright (C) 2021-2022)
 * https://bitbucket.org/staffanulfberg/ldaudio/src/master/
 *
 * This file is part of ld-decode-tools.
 *
 * ld-process-ac3 is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************/

#include <iostream>
#include <getopt.h>
#include <cstring>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#include "../logger.hpp"
#include "../standardoptions.hpp"
#include "OneBitADC.hpp"
#include "Reclocker.hpp"


void doHelp(const std::string &app) {
    std::cout << "Usage: " << app << " [options] source_file output_file [log_file]"
              << "\n  If source_file is '-', stdin  is used."
              << "\n  If output_file is '-', stdout is used."
              << "\n  If log_file    is omitted, stderr is used."
              << "\n"
              << "\n  source_file is expected to provide a stream of 46.08MHz 8-bit unsigned samples."
              << "\n  output_file be overwritten / created with a stream of QPSK symbols."
              << "\n  log_file be overwritten / created with any logging or error messages."
              << "\n  Options:"
              << "\n    -v (int)    Set the logging level. Must be 0-3, representing DEBUG, INFO, WARN and ERR."
              << "\n    -s (int)    Set the sliding average window's size."
              << "\n    --log-level (int)  The same as -v."
              << "\n    --sliding-average-window (int)  The same as -s."
              << "\n    -d, --debug   Show debug messages (the logging level DEBUG; an explicit -v wins)."
              << "\n    -q, --quiet   Suppress info and warning messages (the logging level ERR)."
              << "\n    --version   Print the version."
              << "\n    -h          Print this help."
              << std::endl;
}


int main(int argc, char *argv[]) {
#ifdef _WIN32
    // Set 'binary mode' for stdin and stdout on Windows
    _setmode(_fileno(stdout), O_BINARY);
    _setmode(_fileno(stdin), O_BINARY);	
#endif

    int slidingAvgLength = 1e3;

    // todo 8/16bit and little-/big-endian switches? leave it to sox?
    // todo; allow setting & jumping to start position in file?

    // known issues;
    // small amounts of data left in the buffers at the end

    // The options of the other tools (--help, --version, -d, -q) and the long names are taken first: getopt only knows short options
    if (const int done = takeStandardOptions(argc, argv, "ld-ac3-demodulate", doHelp); done >= 0) return done;

    while (true) {
        switch (getopt(argc, argv, "v:s:h?")) {
            // could have stdin/stdout as defaults, with switches to change them
            case 'v': {
                int level;
                if (!parseWholeNumber(optarg, 0, MAX_LOGLEVEL, level)) {
                    fprintf(stderr, "The logging level must be a whole number from 0 to %d\n", MAX_LOGLEVEL);
                    return 2;
                }
                Logger::GLOBAL_LOG_LEVEL = level;
                continue;
            }
            case 's': // sliding average window size
                if (!parseWholeNumber(optarg, 1, 100000000, slidingAvgLength)) {
                    fprintf(stderr, "The sliding average window must be a whole number of samples, 1 or more\n");
                    return 2;
                }
                fprintf(stderr, "set sliding avg size: %s\n", optarg);
                continue;
            case '?':
            case 'h':
            default:
                doHelp(argv[0]);
                return 2;
            case -1:
                break;
        }
        break;
    }
    int posArgc = argc - optind; // number of positional args
    char **posArgv = &argv[optind]; // array of positional args

    if (posArgc < 2 || posArgc > 3) {
        doHelp(argv[0]);
        return 2;
    }

    std::istream *input = &std::cin;
    std::ostream *output = &std::cout;
    Logger::LOG_STREAM = &std::cerr;

    // Don't force a .flush() on cout when reading from cin
    std::cin.tie(nullptr);

    // prep input file (if not piped)
    // (the buffer of the stream lives as long as the stream: it was declared in the block that opens the file, and was gone, its place on the stack taken by other
    // variables, when the file was read)
    char fileBuffer[8196]; // 8K
    std::ifstream inputFile;
    if (std::strcmp(posArgv[0], "-") != 0) {
        fprintf(stderr, "using input file: %s\n", posArgv[0]);
        inputFile.rdbuf()->pubsetbuf(fileBuffer, sizeof fileBuffer);
        inputFile.open(posArgv[0], std::ostream::binary);
        if (!inputFile.good()) {
            fprintf(stderr, "Cannot open the input file: %s\n", posArgv[0]);
            return 1;
        }
        input = &inputFile;
    }

    // prep output file (if not piped)
    std::ofstream outputFile;
    if (std::strcmp(posArgv[1], "-") != 0) {
        fprintf(stderr, "using output file: %s\n", posArgv[1]);
        outputFile.open(posArgv[1], std::ostream::binary);
        if (!outputFile.good()) {
            fprintf(stderr, "Cannot open the output file: %s\n", posArgv[1]);
            return 1;
        }
        output = &outputFile;
    }

    // prep logger file (if not piped)
    std::ofstream loggerFile;
    if (posArgc > 2 && std::strcmp(posArgv[2], "-") != 0) {
        fprintf(stderr, "using logger file: %s\n", posArgv[2]);
        loggerFile.open(posArgv[2], std::ifstream::binary);
        if (!loggerFile.good()) {
            fprintf(stderr, "Cannot open the log file: %s\n", posArgv[2]);
            return 1;
        }
        Logger::LOG_STREAM = &loggerFile;
    }

    if (!input->good()) {
        fprintf(stderr, "The input cannot be read\n");
        return 1;
    }
    // auto ac3_filter = AC3Filter(*input, sampleFrequency); // now done with sox
    auto adc = OneBitADC(slidingAvgLength, *input); // 1,000 samples sliding average
    // auto resampler = Resampler(sampleFrequency, adc); // 40MHz  // now done with sox

    long qpskSymbols = 0;
    try {
        // (the demodulator reads the samples it begins with when it is made: an input that is too short ends there)
        auto demodulator = Demodulator(adc);
        auto reclocker = Reclocker(demodulator);
        while (true) {
            // could calculate an ETA here?
            // if ((i % 65536) == 0) {
            //     auto logger = Logger(INFO, "INFO");
            //     logger << "Output " << i << " symbols.";
            //     int bytes_parsed = (int) input->tellg();
            //     if (bytes_parsed != -1)
            //         logger << " " << bytes_parsed << " bytes parsed.";
            // }
            uint8_t byte = reclocker.next();
            output->put(char(48 + byte));

            qpskSymbols++;
        }
    } catch (std::range_error &e) {}

    if (qpskSymbols == 0) {
        fprintf(stderr, "The input is too short: there is not a symbol in it\n");
        return 1;
    }

    // print final / overall stats
    Logger(INFO, "QPSK Symbols Total") << qpskSymbols;

    // what was written must have been written
    output->flush();
    if (!output->good()) {
        fprintf(stderr, "Writing the output failed\n");
        return 1;
    }

    // cleanup files nicely
    if (inputFile.is_open())
        inputFile.close();
    if (outputFile.is_open())
        outputFile.close();
    if (loggerFile.is_open())
        loggerFile.close();
    return 0;
}


