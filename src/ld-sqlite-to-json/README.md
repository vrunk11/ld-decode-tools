# ld-sqlite-to-json

**SQLite to JSON Metadata Converter**

## Overview

ld-sqlite-to-json converts TBC metadata from SQLite (`.tbc.db`, as written by current ld-decode) to JSON (`.tbc.json`). It is the reverse of [ld-json-converter](../ld-json-converter/README.md).

Both formats are fully supported by every tool, but the processing tools never convert between them: their output is always in the format of their input. Use this tool when a capture has SQLite metadata and you need JSON, for example for a tool or script that only reads JSON.

## Usage

### Basic Syntax
```bash
ld-sqlite-to-json [options] [input]
```

## Options

### Common Options
- `-h, --help`: Display help on command-line options
- `-v, --version`: Display version information
- `-d, --debug`: Show debug information
- `-q, --quiet`: Suppress info and warning messages

### Input/Output
- `--input-sqlite <filename>`: Specify the input SQLite file (alternatively give it as the positional argument)
- `--output-json <filename>`: Specify the output JSON file (default: the input name with `.db` replaced by `.json`)

## Examples

### Convert the metadata of a capture
```bash
ld-sqlite-to-json capture.tbc.db
```
Writes `capture.tbc.json` next to `capture.tbc.db`.

### Choose the output file
```bash
ld-sqlite-to-json --input-sqlite capture.tbc.db --output-json other.tbc.json
```

## Safety Checks

The tool stops with an error, without writing anything, when:
- the input file does not exist, or is not SQLite metadata (for example a JSON file);
- the output file name ends in `.db`;
- the output file already exists: an existing `.tbc.json` may hold metadata that the SQLite file does not, so it is never overwritten. Remove it first, or choose another name with `--output-json`.

## Notes

- The JSON written is the same as the one the other tools write for JSON metadata. `PAL_M` is written as `PAL-M`, the spelling JSON readers expect.
- If both `capture.tbc.db` and `capture.tbc.json` exist, the processing tools use the `.db` and warn about it; use their `--meta json` option to select the JSON file.

## See Also

- [ld-json-converter](../ld-json-converter/README.md) - JSON to SQLite conversion
