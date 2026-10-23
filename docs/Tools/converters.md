## Converters

Four small programs convert a capture from one form to another. The processing tools never do it (their output is always in the form of their input, see [the metadata formats](../../AGENTS.md)); these do, and nothing else.

| Program | From | To |
| --- | --- | --- |
| `ld-json-converter` | JSON metadata (`.tbc.json`) | SQLite metadata (`.tbc.db`) |
| `ld-sqlite-to-json` | SQLite metadata (`.tbc.db`) | JSON metadata (`.tbc.json`) |
| `ld-tbc-to-cvbs` | a TBC capture (`.tbc` and its metadata) | the CVBS file format read by decode-orc (`.cvbs`, `.meta`, `.dropouts.meta`) |
| `ld-cvbs-to-tbc` | a CVBS capture (as ld-decode or decode-orc write it) | a TBC capture (`.tbc` and `.tbc.db`, or `.tbc.json`) |

A capture whose metadata keep a map (ld-discmap `--save-map`) is converted through it by `ld-tbc-to-cvbs` and `ld-cvbs-to-tbc`, as every tool that reads a capture reads it: the frames of the disc in order, the missing ones blank, the analogue audio in the same order (silence for the missing ones); the metadata written are those of that view. `--ignore-map` converts the capture as it is.

Every option and its default is the one of `--help`; the exit code is 0 when the work was done, 2 for a wrong command line and 1 when it failed.

The metadata converters take their files with `--input-metadata` and `--output-metadata`, as the other tools do. The original names (`--input-json` and `--output-sqlite` of ld-json-converter, `--input-sqlite` and `--output-json` of ld-sqlite-to-json and ld-export-decode-metadata) are still accepted, and not shown in `--help`; giving a file under both names is an error.

### ld-json-converter

Converts the metadata of a capture from JSON to SQLite, written with the schema of ld-decode. An existing output file is replaced. A JSON file that has no blanking level (a NTSC one from before it was kept) gets the one the library works out from the levels of the video system, not a copy of the black level.

```
ld-json-converter [options]

  --input-metadata <filename>  Specify the input JSON file
  --output-metadata <filename>  Specify the output SQLite file (default same as
                              input but with .db extension)
```

### ld-sqlite-to-json

Converts the metadata of a capture from SQLite to JSON, the other way round.

```
ld-sqlite-to-json [options] input

  --input-metadata <filename>  Specify the input SQLite file
  --output-metadata <filename>  Specify the output JSON file (default same as
                             input but with .json extension)

  input                      Input SQLite file (if --input-metadata is not given)
```

### ld-tbc-to-cvbs

Converts a TBC and its metadata to the CVBS file format: `<name>.cvbs`, `<name>.meta` and `<name>.dropouts.meta`, plus `<name>.cvbs.db` (or `.cvbs.json`) which keeps the field metadata that CVBS has no place for. A Y/C capture (`<name>.tbc` with `<name>_chroma.tbc`) becomes `<name>.cvbsy` and `<name>.cvbsc`.

```
ld-tbc-to-cvbs [options] input output

  --meta, --metadata-format <db|json>  Metadata format to use: db (SQLite
                                       <input>.db) or json (<input>.json).
                                       Default: db if present, otherwise json
  --input-metadata <filename>          Specify the input metadata file (default
                                       input.db or input.json, see --meta)
  -e, --encoding <encoding>            Sample encoding: u10 (CVBS_U10_4FSC,
                                       default), u16, tpg21 or s16
  --ignore-map                         Read the capture as it is, though its
                                       metadata keep a map (the frames in the
                                       order of the disc, from ld-discmap)
  -r, --samplerate <Hz>                Sample rate of the audio in Hz (default:
                                       from the metadata, else 44100). The audio
                                       is copied as it is, not resampled

  input                                Specify input TBC file
  output                               Specify output CVBS file (default
                                       input.cvbs)
```

### ld-cvbs-to-tbc

Converts a CVBS capture to `<name>.tbc` with `<name>.tbc.db` metadata (or `.tbc.json`). A Y/C pair (`<name>.cvbsy` and `<name>.cvbsc`) becomes `<name>.tbc` and `<name>_chroma.tbc`, or `<name>.tbcy` and `<name>.tbcc` (as decode-orc names them) if the output file is named `<name>.tbcy`.

```
ld-cvbs-to-tbc [options] input output

  --meta, --metadata-format <db|json>  Metadata format to create: db (SQLite
                                       <output>.db, the default) or json
                                       (<output>.json)
  --ignore-map                         Read the capture as it is, though its
                                       metadata keep a map (the frames in the
                                       order of the disc, from ld-discmap)
  -r, --samplerate <Hz>                Sample rate of the audio in Hz (default:
                                       that of the WAV file, normally 48000).
                                       The audio is copied as it is, not
                                       resampled

  input                                Specify input CVBS file (.cvbs, or
                                       .cvbsy for Y/C)
  output                               Specify output TBC file (default
                                       input.tbc)
```

In these two programs `-r` is the sample rate of the audio, not the field order as in the processing tools.

All four also take the options common to the tools: `-h`, `-v`, `-d` (debug messages) and `-q` (quiet).
