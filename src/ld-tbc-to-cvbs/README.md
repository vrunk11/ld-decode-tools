# ld-tbc-to-cvbs

**TBC to CVBS Converter**

## Overview

ld-tbc-to-cvbs converts a TBC capture (`.tbc` with `.tbc.db` or `.tbc.json` metadata) to the
[CVBS file format](https://github.com/decode-orc/cvbs-file-format-specification) that ld-decode 8 and
decode-orc use. It is the reverse of [ld-cvbs-to-tbc](../ld-cvbs-to-tbc/README.md).

For `capture.tbc` it writes:
- `capture.cvbs` - the video as whole frames (first field, then second)
- `capture.meta` - the CVBS core metadata (video standard, sample encoding, frame count, ...)
- `capture.dropouts.meta` - the dropouts, in the CVBS dropout extension format
- `capture.cvbs.db` (or `capture.cvbs.json` for JSON input) - every field's metadata (VBI, VITS,
  dropouts, ...) in the usual ld-decode format, which the CVBS core metadata has no place for

A Y/C capture (`capture.tbc` with `capture_chroma.tbc`, or decode-orc's `capture.tbcy` with
`capture.tbcc`) becomes `capture.cvbsy` and `capture.cvbsc`.

The TBC levels are mapped onto the standard levels of the video system, as decode-orc does, so the
output is the same as decode-orc's own conversion. When the metadata has no blanking level (older JSON
metadata), it is worked out from the black level as decode-orc does: on the 7.5 IRE setup for NTSC and
PAL-M, or at black for NTSC-J. A field without a partner (a capture starting or ending half-way
through a frame) is left out, since CVBS files hold whole frames.

The `.pcm` audio is not converted (the CVBS format carries 48 kHz WAV channel pairs).

## Usage

### Basic Syntax
```bash
ld-tbc-to-cvbs [options] <input> [output]
```

## Options

### Common Options
- `-h, --help`: Display help on command-line options
- `-v, --version`: Display version information
- `-d, --debug`: Show debug information
- `-q, --quiet`: Suppress info and warning messages

### Metadata
- `--meta <db|json>`: Read `input.db` or `input.json` when both exist (default: `.db`)
- `--input-metadata <filename>`: Specify the input metadata file

### Output
- `-e, --encoding <encoding>`: Sample encoding of the output: `u10` (`CVBS_U10_4FSC`, the default and
  decode-orc's), `u16` (`CVBS_U16_4FSC`, as ld-decode writes), `tpg21` or `s16`

## Arguments

- `input`: The TBC file
- `output`: The CVBS file (default: the input name with `.cvbs`, or `.cvbsy` for Y/C)

## Examples

### Convert a capture
```bash
ld-tbc-to-cvbs capture.tbc
```

### Use the encoding ld-decode writes
```bash
ld-tbc-to-cvbs -e u16 capture.tbc other.cvbs
```

## Notes

- Only 4fsc captures convert (1135 x 313 fields for PAL, 910 x 263 for NTSC, 909 x 263 for PAL-M).
- Existing output files are never overwritten.
- A CVBS capture converted to TBC and back in the same encoding keeps its samples, apart from PAL's
  four extra samples per frame and values a TBC cannot hold (below 0 or above 1023 in 10 bits).
