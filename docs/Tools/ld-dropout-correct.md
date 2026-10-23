## ld-dropout-correct
This application uses the dropout information in the metadata file (`.tbc.db` or `.tbc.json`) to perform dropout correction on the input TBC file and produces a new output file.  The current version of the corrector uses framing in order to provide inter-field correction.  Note that inter-field correction may not function correctly for NTSC pull-down sources.

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
  --ignore-map                         Read the capture as it is, though its
                                       metadata keep a map (the frames in the
                                       order of the disc, from ld-discmap)
  --no-audio                           Do not write the analogue audio of the
                                       first source with the video (an LDX
                                       output has it by default)
  --copy-streams <streams>             The other streams of the sources (EFM, AC3,
                                       RF, 24-bit audio) to copy as they are into
                                       an LDX output, with their own times: none,
                                       default (the EFM, AC3 and 24-bit audio of
                                       the first source), or a list such as
                                       1:efm,1:ac3,2:#5 (the number of the source
                                       from 1, then efm, ac3, rf, audio, all or
                                       #track)
  -o, --overcorrect                    Over correct mode (use on heavily
                                       damaged single sources)
  --overcorrect-extension <samples>    Over correct mode, with the dropouts
                                       extended by this many samples at each end
                                       (0 to 48; -o is 24)
  --max-distance <lines>               Take no replacement from a line more
                                       than this many frame lines from the
                                       dropout (default: no limit)
  --highlight                          Fill the parts that were corrected with
                                       the white level, to see what was
                                       corrected (a check: the output is not for
                                       use)
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

With `-` as the output file (and no `--output-metadata`), the output is an LDX stream on the standard output, with the metadata of each field just before its video and the history of the source and of this tool, so that the next tool can read it (`ld-container pack capture.tbc -o - | ld-dropout-correct ... - | ld-analyse -`). A raw TBC on the standard output is still written with `--output-metadata`. CVBS video is sent as `cvbs` frames (the 24-bit audio of a CVBS capture, for `ld-discmap`, as a 24-bit `pcm` track).

## Output as an LDX file

When the output is an LDX file, it holds the analogue audio of the **first source** (the reference) with the video, as a `pcm` track: the audio of the fields that the first source gives (for a source read through a map, the audio of the field of the capture each field is, silence for a padded frame). `--no-audio` leaves it out. **The other streams** of the sources (the EFM and AC3 tracks, the RF, the 24-bit audio of a CVBS capture) are copied as they are, block by block, with the times they have in their source, by `--copy-streams`: by default those of the first source, `none` for none, or a list (`1:efm,2:#5`). They are **not realigned** to the frames of the output, and the history of the file says what was copied from where, and that it was not realigned, but **the offset is recorded**: each copied track has an `alignment` parameter (for each stretch of frames of the output, the frame of its source that goes with it: a shift, a skip, frames that the source does not have), and the history says whether the stream is shifted from the video, by how many frames, or in how many pieces, so that a reader can correct the offset between the video and the stream. A `.tbc` output has no audio and no other stream, as before (copy the `.pcm` of the source you want).
