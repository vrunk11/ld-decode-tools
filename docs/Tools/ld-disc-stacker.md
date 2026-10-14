## ld-disc-stacker
This application performs 'stacking' of multiple versions of the same disc (ideally different copies of discs containing the same mastered contents).  Disc stacking requires a minimum of two input sources in order to work (although 3 or more are strongly recommended).

Stacking is performed by taking all available input sources and, pixel by pixel, determining the number of available sources for the pixel (by removing outliers marked as dropouts in the source files).  If more than 3 sources are available for the pixel the tool will use the median value of the available sources as the output (when there are an even number available of pixel sources, the two centre values of the median are averaged to the output).  If only two sources are available an average is used.  If only one source is available it is passed directly as the output.

If no sources are available for a pixel the tool will attempt to use differential dropout detection (diffDOD) to recover a value (in the case where ld-decode marked the pixel as a false-negative) unless the --no-diffdod option is specified.

If the --passthrough option is specified the tool will, when all pixel sources are marked as a dropout, mark the resulting output pixel as a dropout (regardless of the diffDOD result).  The --passthrough is useful in non-preservation cases where it is desirable for master plate errors (which cause dropouts in the same place on all resulting LaserDisc copies) to be marked for dropout concealment (where-as diffDOD would correctly identify the plate error as present on all discs (and therefore not an error)).

Use the tool by specifying the available input .tbc files followed by the desired .tbc output file.

```
Options:
  -v, --version                        Displays version information.
  -d, --debug                          Show application debug messages
  -q, --quiet                          Suppress info and warning messages
  -?, -h, --help                       Displays help on commandline options.
  --help-mode                          Show info about stacking mode
  -V, --verbose                        Show more info during stacking
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
  -t, --threads <number>               Specify the number of concurrent threads
                                       (default is the number of logical CPUs)
  -m, --mode <number>                  Specify the stacking mode to use
                                       (default is Auto) -1 = auto / 0 = mean /
                                       1 = median / 2 = smart mean / 3 = smart
                                       neighbor / 4 = neighbor
  --st, --smart-threshold <number>     Specify the range of value in 8 bit
                                       (0~128) for selecting sample where the
                                       distance to the median didn't exceed the
                                       selected value for applying mean (default
                                       is 15)
  --no-diffdod                         Do not use differential dropout
                                       detection on low source pixels
  --no-map                             Disable mapping requirement
  --passthrough                        Pass-through dropouts present on every
                                       source
  --it, --integrity                    Check if frames contain skip or sample
                                       drop and discard bad source for specific
                                       frame

Arguments:
  inputs                               Specify input TBC files (- as first
                                       source for piped input)
  output                               Specify output TBC file (omit or - for
                                       piped output)
```

## Stacking a session file

Instead of the list of inputs, one `.stack` session file can be given, as saved by [ld-stacker-gui](ld-stacker-gui.md):

```
ld-disc-stacker session.stack [output.tbc]
```

The sources that are switched on in the session are stacked, with the stacking options and the output file of the session; what is given on the command line (such as `--mode` or `--smart-threshold`) takes precedence, and an output file after the session replaces the one in it. At least 2 sources must be on.

The sources are not matched by their VBI frame numbers but lined up by an offset in frames each. The first source that is on is the reference: the stack has its frames, and its offset means nothing. The frame `f` of the stack is made from the frame `f - offset` of every other source, which has no frame there if that falls outside it. This is for captures that cannot be mapped; it needs no VBI data and no mapping with ld-discmap.

With no offsets, a session gives the same stack as the usual way with `--no-map` when the sources are in step.

A session can also be read without stacking it to a file: ld-analyse opens a `.stack` file and shows the stack as the frames are asked for, and ld-chroma-decoder decodes one, with the same picture as the file this tool writes.
