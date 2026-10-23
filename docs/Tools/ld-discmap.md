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
  -u, --delete-unmappable-frames       Delete unmappable frames (by default
                                       they are kept, given a number and
                                       marked as dropouts)
  -n, --no-audio                       Do not process analogue audio
  --legacy                             Write the mapped capture (a copy of the
                                       video, the audio and the metadata) as
                                       ld-discmap always did, instead of putting
                                       the map in a .stack session
  --save-map                           Keep the map in the metadata of the input
                                       (the .tbc.db, the .tbc.json, or the LDX
                                       file): the tools that read the capture
                                       then read it in the order of the disc.
                                       Alone, with the input only, nothing else
                                       is written
  --map-file <file>                    Write the map of the capture (the frames
                                       in the order of the disc, and where each
                                       number comes from) as JSON to this file
  --gap-allowance <frames>            A gap in the numbers is padded only up
                                       to the frames of the capture between the
                                       two frames plus this many (a number that
                                       was misread makes a wide gap that the
                                       capture does not account for; default
                                       100)
  --meta, --metadata-format <db|json>  Metadata format to use: db (SQLite
                                       <input>.db) or json (<input>.json).
                                       Default: db if present, otherwise json

Arguments:
  input                                Specify input TBC file
  output                               Specify the .stack session to put the
                                       map of the input in (with --legacy: the
                                       output TBC file)
```

## The map, without a copy

ld-discmap works out, for each frame of the disc, which frame of the capture goes there and where its number comes from. The mapped capture it used to write is that map applied: the same fields in another order, with the numbers written in the VBI. The map alone is small (a few kilobytes, a line for each skip), and the tools that read a capture read it through the map. **This is what ld-discmap does now**:

- `ld-discmap capture.tbc session.stack` puts the map in the session, as the `map` of the source that is the capture (the source is added if the session has none for it, and a session that does not exist is made); no capture is written. `ld-analyse`, `ld-chroma-decoder`, `ld-disc-stacker` and `ld-stacker-gui` given the session read the capture through its map, and give the same result as with the mapped copy (checked on a capture of 2 624 frames, with skips and frames that could not be numbered: the decoded picture is the same byte for byte). The map says, frame by frame, whether the number was measured from the VBI, replaced by the sequence, deduced from the pulldown, guessed, or padded (see the specification of the container, section 13). Several captures are mapped one after the other into the same session.
- **`--save-map`** keeps the map in the metadata of the capture: a SQLite file gets a table `disc_map`, a JSON file a member `ldxMap`, an LDX file new metadata with the map (a backup of a metadata file is made, `name.bup`). From then on **every tool that reads the video of the capture reads it through the map**: `ld-chroma-decoder`, `ld-disc-stacker`, `ld-dropout-correct`, `ld-analyse` and `ld-stacker-gui` (`--ignore-map` reads the capture as it is; ld-discmap, ld-process-vbi, ld-process-vits and the converters work on the capture itself). `ld-discmap --save-map capture.tbc` writes nothing else.
- `--map-file <file>` writes the map as JSON (with `--maponly`, nothing else is written).
- **`--legacy`** writes the mapped capture as ld-discmap always did (`ld-discmap --legacy capture.tbc capture_map.tbc`): a copy of the video, the audio and the metadata, which tools that do not read sessions (ld-decode, decode-orc, scripts) can use. Without `--legacy`, an output that is not a `.stack` session is refused.

## Gaps that the capture does not account for

A gap in the numbers is padded with black frames only when the capture can account for it: each frame that the capture holds between the two frames is one missing picture the mapping could not place, and `--gap-allowance` (100 frames by default) covers a player that mistracked forward over pictures the capture never recorded. A number that was misread makes a wide gap with next to nothing of the capture in it: it is not padded, and ld-discmap says so (the mapped capture used to grow by the width of the gap).

## Where a number comes from

For a CAV disc the picture number is on two lines of the VBI. When the two agree the frame is **measured**; when only one could be read, or they disagree (the number of line 18 is kept), it is **unconfirmed** in the map. A CLV time code is always counted as measured.

## NTSC CLV discs

The time code of an NTSC CLV disc jumps by one number at regular places (the IEC amendment 2, as the drop-frame time code does). ld-discmap knows these places and does not take the missing number for a lost frame, so it pads nothing there. The stacking tools (ld-disc-stacker, ld-dropout-correct) and the stacking sessions do not know them: they line the sources up by the position of the frames in the mapped captures, which is right for what ld-discmap wrote; aligning NTSC CLV captures by their disc numbers (with `--no-map`) can be one frame out after each of these places.

## Which copy of a repeated frame is kept

When the capture holds the same frame more than once (the player went back), the copy that is kept is the one with the best **quality**, from 0 to 100: the black PSNR of the two fields (as a share of 43 dB, in amplitude), a penalty if the number of the next frame goes back, the sync confidence, and the share of the frame that is not in a dropout. The last counts 1000 times more than the others, so the copy with the fewest dropouts is kept, and the others break a tie. The black PSNR is read from the metadata, where `ld-process-vits` writes it, and is 0 for every frame without it; it is 1 part of 1004, so it only breaks a tie between copies that have the same dropouts, and the map itself (numbers, order, gaps) never depends on it.

## Unmappable frames

A frame that has no number and cannot be given one (the VBI is unreadable over several frames) does not make ld-discmap fail. It is **kept**, given a number and **marked as a dropout**:

- it takes the next number that is free after the frame before it, if there is room before the number of the frame after it, so it stays where it was and no other frame shifts;
- if there is no room, it takes the number 0, which no real frame has, and goes before the first frame, as a lead-in does (a CAV frame 0 is a lead-in frame to ld-discmap and to the stack);
- its fields are written with a dropout over their whole width on every line, so that the stack leaves the frame out as it does a dropout.

The log says how many frames were numbered in place and how many were numbered 0. `--delete-unmappable-frames` deletes them instead: they are dropped from the map and their place is padded like any other gap.

## LDX files

The input can be an LDX file (`capture.ldx`, or `capture.ldx#3` for one of its tracks): the video and the metadata are read from it, and the analogue audio from its `pcm` track. The output stays TBC and PCM files.

### LDX output

The output can be an LDX file (`out.ldx`): the video is a track of it, with a track for the metadata (the tool adds them when it is done) and, for `ld-discmap`, one for the analogue audio instead of the `.pcm` file. The history of the file records the work, with the arguments of the tool (`ld-container history out.ldx`).

With `-` as the output file, the output is an LDX stream on the standard output, with the metadata of each field just before its video and the history of the source and of this tool, so that the next tool can read it (`ld-container pack capture.tbc -o - | ld-discmap ... - | ld-analyse -`). CVBS video is sent as `cvbs` frames (the 24-bit audio of a CVBS capture, for `ld-discmap`, as a 24-bit `pcm` track).
