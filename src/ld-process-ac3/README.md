# ld-process-ac3/ld-ac3-*

**AC3 Audio Decoder for LaserDisc RF Signals**

## Overview

ld-process-ac3/ld-ac3-* tools decode AC3 Audio.

## Building

The tools are built and installed with the rest of ld-decode-tools (see [BUILD.md](../../BUILD.md)). They use POSIX `getopt`; with MSVC on Windows it comes from vcpkg's `getopt` port, which `build.bat` and the CI install from `vcpkg.json`.

## Usage

### Basic Syntax
```bash
ld-ac3-demodulate [options] source_file output_file [log_file]
ld-ac3-decode [options] source_file output_file [log_file]
```

## Options

### ld-ac3-demodulate
#### Options
- `-v <int>`, `--log-level <int>`: Set the logging level (0-3: DEBUG, INFO, WARN, ERR)
- `-s <int>`, `--sliding-average-window <int>`: Set the sliding average window's size
- `-d`, `--debug`: Show debug messages (the logging level DEBUG; an explicit `-v` wins)
- `-q`, `--quiet`: Suppress info and warning messages (the logging level ERR)
- `--version`: Print the version
- `-h`, `--help`: Print help information

#### Arguments
- `source_file`: Input RF signal file, 46.08 MHz 8-bit unsigned samples (use '-' for stdin)
- `output_file`: Output QPSK symbols file (use '-' for stdout)
- `log_file`: Optional log file (defaults to stderr)

### ld-ac3-decode
#### Options
- `-v <int>`, `--log-level <int>`: Set the logging level (0-3: DEBUG, INFO, WARN, ERR)
- `-d`, `--debug`: Show debug messages (the logging level DEBUG; an explicit `-v` wins)
- `-q`, `--quiet`: Suppress info and warning messages (the logging level ERR)
- `--version`: Print the version
- `-h`, `--help`: Print help information

#### Arguments
- `source_file`: Input QPSK symbols file (use '-' for stdin)
- `output_file`: Output AC3 audio file (use '-' for stdout)  
- `log_file`: Optional log file (defaults to stderr)
