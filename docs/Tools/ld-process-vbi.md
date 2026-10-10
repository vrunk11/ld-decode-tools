## ld-process-vbi

This application detects and extracts metadata from the vertical blanking interval for each field in an input TBC file, and adds the metadata into the TBC's metadata file, `.tbc.db` or `.tbc.json` (or, with `--output-metadata`, to a new file in the same format).

It currently supports:

* LaserDisc biphase code
* LaserDisc FM code and white flag (NTSC only)
* Closed Captions (NTSC only)
* Vertical Interval Timecode

While ld-decode and vhs-decode extract some of this information during decoding, ld-process-vbi supports more formats and does more thorough sanity-checking, so it's a good idea to run it after decoding and before further processing. If you are stacking multiple captures, you should run it again on the stacked TBC file to update the metadata.

Syntax:

ld-process-vbi \<options> \<input TBC file name>

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
  --input-metadata <filename>          Specify the input metadata file (default
                                       input.db or input.json, see --meta)
  --output-metadata <filename>         Specify the output metadata file
                                       (default same as input)
  -n, --nobackup                       Do not create a backup of the input
                                       metadata
  -t, --threads <number>               Specify the number of concurrent threads
                                       (default is the number of logical CPUs)

Arguments:
  input                                Specify input TBC file
```
