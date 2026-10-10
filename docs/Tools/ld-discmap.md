## ld-discmap
This application scans through the metadata for a .tbc file and attempts to correct metadata and video data to ensure the .tbc contains the correct amount of frames, in the right order, without duplication.  The application also pads the .tbc with dummy (black) frames when one or more frames are missing.

Disc mapping is the process of mapping out a .tbc file to ensure that the VBI frame numbering is correct and there are no missing or repeating frames in the .tbc. This is useful for stacking since all source copies must be 'aligned' so the stacker knows which frames are identical across all source copies.

As there can be many capture issues (especially with damaged/rotten discs) combined with complexities such as pulldown frames (which are not numbered in the VBI) - disc mapping is not a fool-proof task. The ld-discmap tool is designed to warn you if things don't look right, but it can't spot every possible issue.

It is recommended to run ld-process-vbi on the input .tbc before running this tool.

```
Options:
  -?, -h, --help                       Displays help on commandline options.
  --help-all                           Displays help, including generic Qt
                                       options.
  -v, --version                        Displays version information.
  -d, --debug                          Show application debug messages
  -q, --quiet                          Suppress info and warning messages
  -r, --reverse                        Reverse the field order to second/first
                                       (default first/second)
  -m, --maponly                        Only perform mapping - No output TBC
                                       file required
  -s, --nostrict                       No strict checking on pulldown frames
  -u, --delete-unmappable-frames       Delete unmappable frames
  -n, --no-audio                       Do not process analogue audio
  --meta, --metadata-format <db|json>  Metadata format to use: db (SQLite
                                       <input>.db) or json (<input>.json).
                                       Default: db if present, otherwise json

Arguments:
  input                                Specify input TBC file
  output                               Specify output TBC file
```
