# ld-cvbs-to-tbc

**CVBS to TBC Converter**

## Overview

ld-cvbs-to-tbc converts a capture in the
[CVBS file format](https://github.com/decode-orc/cvbs-file-format-specification), as written by
ld-decode 8 or decode-orc, to a TBC capture (`.tbc` with `.tbc.db` or `.tbc.json` metadata). It is the
reverse of [ld-tbc-to-cvbs](../ld-tbc-to-cvbs/README.md).

All the tools read CVBS captures directly, so converting is only needed for other software that reads
TBC files.

For `capture.cvbs` it writes `capture.tbc` and `capture.tbc.db`. The metadata comes from the companion
`capture.cvbs.db` / `capture.cvbs.json` when there is one (keeping every field's metadata), otherwise
from `capture.meta` and `capture.dropouts.meta`. A Y/C pair (`capture.cvbsy` and `capture.cvbsc`)
becomes `capture.tbc` and `capture_chroma.tbc`.

The CVBS audio (`capture_audio_N.wav`) is not converted to `.pcm`.

## Usage

### Basic Syntax
```bash
ld-cvbs-to-tbc [options] <input> [output]
```

## Options

### Common Options
- `-h, --help`: Display help on command-line options
- `-v, --version`: Display version information
- `-d, --debug`: Show debug information
- `-q, --quiet`: Suppress info and warning messages

### Metadata
- `--meta <db|json>`: Format of the TBC metadata to write (default: that of the companion, or SQLite)

## Arguments

- `input`: The CVBS file (`.cvbs`, or either file of a `.cvbsy` / `.cvbsc` pair)
- `output`: The TBC file (default: the input name with `.tbc`)

## Examples

### Convert a capture
```bash
ld-cvbs-to-tbc capture.cvbs
```

### Write JSON metadata
```bash
ld-cvbs-to-tbc --meta json capture.cvbs other.tbc
```

## Notes

- Only time-base stable 4fsc captures (`STANDARD_STABLE_LOCKED` or `STANDARD_STABLE_UNLOCKED`) convert.
- TBC samples are unsigned 16-bit, so values below zero (`CVBS_U10_4FSC` headroom under sync tip) become
  0, and PAL's four extra samples per frame are dropped.
- Existing output files are never overwritten.
