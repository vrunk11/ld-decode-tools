## ld-export-decode-metadata

Exports the SQLite metadata of a capture (`.tbc.db`) to a JSON file for external tools. The SQLite format is internal to the ld-decode tools and can change; the exported JSON has a documented layout with a version field, so a script should read that instead of the database. (This is not the JSON metadata that the other tools read and write: for that, see [ld-sqlite-to-json](converters.md).)

Syntax:

ld-export-decode-metadata \<options>

```
Options:
  -?, -h, --help             Displays help on commandline options.
  --help-all                 Displays help, including generic Qt options.
  -v, --version              Displays version information.
  -d, --debug                Show application debug messages
  -q, --quiet                Suppress info and warning messages
  --input-sqlite <filename>  Specify the input SQLite file
  --output-json <filename>   Specify the output JSON file (default same as
                             input but with -export.json extension)
```

The layout of the exported file is described in the [README of the tool](../../src/ld-export-decode-metadata/README.md).
