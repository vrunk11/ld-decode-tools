## ld-process-vits


This application performs an analysis of the VITS (Vertical Interval Test Signals) and recalculates the white and black SNR values in the metadata.  Note that any TBC file will already have this metadata provided by ld-decode.  This tool is generally used along with dropout correction and stacking as the video content of the TBC is modified - running ld-process-vits therefore allows you update the SNR metadata in order to analyse the result (using ld-analyse).

Use the tool by specifying the required input .tbc file.  The tool will backup the previous metadata file (to .vbup) before proceeding.

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

## LDX files

The input can be an LDX file (`capture.ldx`, or `capture.ldx#3` for one of its tracks). The new metadata are not written beside the file: they are added to the LDX file as the current metadata of the capture (a block added at the end, which supersedes the one that was there), and the history of the file records it. If they cannot be written the tool says so and ends with an error.

### In a pipe

`-` as the input is an LDX stream on the standard input, and the LDX stream goes out on the standard output with the VITS metrics of each field added to its metadata, each field sent as soon as it is done (the metadata of a field in a block of its own just before its video), so that tools can follow one another:

```
ld-container pack capture.tbc -o - | ld-process-vbi - | ld-process-vits - | ld-analyse -
```

A stream without metadata gets the defaults of its video system. The history of the stream records the tool. See the specification of the container format (section 15).
