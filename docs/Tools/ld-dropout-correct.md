## ld-dropout-correct
This application uses the drop-out information in the metadata file (`.tbc.db` or `.tbc.json`) to perform dropout correction on the input TBC file and produces a new output file.  The current version of the corrector uses framing in order to provide inter-field correction.  Note that inter-field correction may not function correctly for NTSC pull-down sources.

Syntax:

ld-dropout-correct \<options> \<input TBC file name> \<output TBC file name>

```
Options:
  -?, -h, --help                       Displays help on commandline options.
  --help-all                           Displays help, including generic Qt
                                       options.
  -v, --version                        Displays version information.
  -d, --debug                          Show application debug messages
  -q, --quiet                          Suppress info and warning messages
  --meta, --metadata-format <db|json>  Metadata format to use: db (SQLite
                                       <input>.db) or json (<input>.json).
                                       Default: db if present, otherwise json
  --input-metadata <filename>          Specify the input metadata file for the
                                       first input file (default input.db or
                                       input.json, see --meta)
  --output-metadata <filename>         Specify the output metadata file
                                       (default output.db, or output.json for
                                       JSON input)
  -r, --reverse                        Reverse the field order to second/first
                                       (default first/second)
  -o, --overcorrect                    Over correct mode (use on heavily
                                       damaged single sources)
  -i, --intra                          Force intrafield correction (default
                                       interfield)
  -t, --threads <number>               Specify the number of concurrent threads
                                       (default is the number of logical CPUs)

Arguments:
  inputs                               Specify input TBC files (- as first
                                       source for piped input)
  output                               Specify output TBC file (omit or - for
                                       piped output)
```

The input can be an LDX file (`capture.ldx`, or `capture.ldx#3`): the video and the metadata are read from it, and the corrected video goes to a TBC file.

### From a pipe

The first source can be `-`, an LDX stream on the standard input, without `--input-metadata`: the video and the metadata come from the stream. Correcting needs the metadata of all the fields (VBI and mapping) before the video, so a stream whose metadata come field by field is refused. The output is a TBC file.

### LDX output

The output can be an LDX file (`out.ldx`): the video is a track of it, with a track for the metadata (the tool adds them when it is done) and, for `ld-discmap`, one for the analogue audio instead of the `.pcm` file. The history of the file records the work, with the arguments of the tool (`ld-container history out.ldx`).

With `-` as the output file (and no `--output-metadata`), the output is an LDX stream on the standard output, with the metadata of each field just before its video and the history of the source and of this tool, so that the next tool can read it (`ld-container pack capture.tbc -o - | ld-dropout-correct ... - | ld-analyse -`). A raw TBC on the standard output is still written with `--output-metadata`. CVBS video cannot be sent as a stream yet.
