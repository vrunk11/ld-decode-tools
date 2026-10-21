## ld-process-ac3

Two programs decode the Dolby Digital (AC3) audio that a LaserDisc carries in its RF signal, in two steps. They read and write streams (`-` is the standard input or output) and take no metadata.

```
ld-ac3-demodulate [options] source_file output_file [log_file]
ld-ac3-decode [options] source_file output_file [log_file]
```

1. `ld-ac3-demodulate` reads the RF sampled at 46.08 MHz (8-bit unsigned samples) and writes a stream of QPSK symbols.
2. `ld-ac3-decode` reads the QPSK symbols and writes the AC3 stream.

If `log_file` is left out, the messages go to the standard error.

```
ld-ac3-demodulate options:
    -v (int), --log-level (int)    Set the logging level. Must be 0-3, representing DEBUG, INFO, WARN and ERR.
    -s (int), --sliding-average-window (int)    Set the sliding average window's size.
    -d, --debug   Show debug messages (the logging level DEBUG; an explicit -v wins).
    -q, --quiet   Suppress info and warning messages (the logging level ERR).
    --version     Print the version.
    -h, --help    Print this help.

ld-ac3-decode options:
    -v (int), --log-level (int)    Set the logging level. Must be 0-3, representing DEBUG, INFO, WARN and ERR.
    -d, --debug   Show debug messages (the logging level DEBUG; an explicit -v wins).
    -q, --quiet   Suppress info and warning messages (the logging level ERR).
    --version     Print the version.
    -h, --help    Print this help.
```

These two programs read their options with POSIX `getopt` (the options with a long name above are taken from the command line before it), and they keep their own exit codes (see section 4.4 of AGENTS.md). Here `-v` is the logging level, not the version.
