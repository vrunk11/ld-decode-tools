# Findings of step 2 (branch `audit-refactor`)

How they are found and worked on: [audit-step2-plan.md](audit-step2-plan.md). Status: `to verify` (read, not proven or measured), `confirmed` (read and proven or measured),
`decided: <what>`, `done`, `dropped: <why>`. First batch, 2026-10-22: the first four workflows read in part; more follow.

## Workflow 3 - putting a capture in order of the disc

### S-01 - Mapping writes the whole capture again
- **kind**: suboptimal and impractical. **status**: direction decided (2026-10-22): the map in the session and in an LDX track, the copy kept as `--copy`; details in audit-step2-mapping.md; confirmed (read).
- **where**: `ld-discmap/discmapper.cpp:713` (`saveDiscMap`): every field of the result is read from the input and written to a new TBC, with the audio copied beside it and the
  metadata written again. `ld-stacker-gui/mapdialog.cpp` runs it and puts the result next to the capture (`name_map.tbc`, or a mapped track of an LDX file with `mappedFrom`).
- **what**: what the mapping decides is small: for each frame of the disc, which frame of the capture goes there (or none: a padded frame), and the numbers. That is a list of
  integers. The video of the result is the same samples in another order, with black fields for the gaps.
- **effect**: time and disk (a mapped capture is another copy of the capture: tens of gigabytes for a disc); two captures to keep in step (`_map` is found by its name); a mapping
  cannot be undone or tried again without writing another copy. Correction: `ld-disc-stacker --no-map` does not map in memory (it drops the requirement and lines frames up by the first number), but `StackedVideo` already reads a capture through a table of positions, and `DiscMap::buildTargetMetadata` builds the mapped metadata in memory: see [audit-step2-mapping.md](audit-step2-mapping.md).
- **proposal** (to be decided): the mapping kept as **metadata**: a list of source frames, in the `.tbc.db` (a table), in the `.ldx` (a track of its own), or as a `.stack`-like file, that
  `SourceVideo` applies when it reads (it already has a virtual layer for stacks). The mapped copy stays available as an export. **risk**: high: it changes how every tool reads a mapped
  capture, and the specification of the container; but it would remove the biggest copy of the workflow.
- **to find out**: which tools would have to know the map (all that read a TBC through `SourceVideo`: it could be hidden there), and what ld-decode and decode-orc do with a mapped file.

### S-12 - The names of the files around a mapping, and their backups
- **kind**: impractical and illogical. **status**: partly done (2026-10-23); confirmed (read; the backup refusal run).
- **where**: `library/tbc/tbcfiles.cpp` (`mappedFileName`: `<name>_map.tbc`), `library/tbc/lddecodemetadata.cpp` (`metadataFileName`, `findMetadataFile`), `ld-stacker-gui/mapdialog.cpp`
  (`temporaryFor`, `companions`, `replaceOriginal`), `library/tbc/commonoptions.cpp` (`NoBackupOption::backUp`), the mains of `ld-process-vbi` and `ld-process-vits`.
- **what**: what is beside a capture after it has been mapped and measured, as seen in a working folder of the person (`decode.tbc`, `decode.tbc.json`, `decode.pcm`, `decode_map.tbc`,
  `decode_map.tbc.json`, `decode_map.pcm`) and in the code:
  | what | name | made by |
  |---|---|---|
  | the metadata of a TBC | `<name>.tbc.db` or `<name>.tbc.json` (the suffix is added to the whole name) | ld-decode, the tools |
  | the metadata of a CVBS capture | `<name>.meta` and `<name>.dropouts.meta` (the suffix replaces `.cvbs`), and our companion `<name>.cvbs.db` / `.cvbs.json` | decode-orc / ld-decode 8, the tools |
  | the audio | `<name>.pcm` (replaces `.tbc`) for a TBC, `<name>_audio_0.wav` for CVBS | ld-decode, decode-orc |
  | a mapping kept beside the capture | `<name>_map.tbc`, `<name>_map.tbc.json` or `.db`, `<name>_map.pcm` (the stacking tools find it **by this name**) | ld-discmap, the Map window |
  | a mapping put in the place of the capture | first `<name>.mapping-tmp.tbc` and `<name>.mapping-tmp.tbc.db` (or `.json`, `.pcm`), then the capture and its files renamed `<file>.bak`, the mapping renamed, and the `.bak` **deleted** | the Map window |
  | the same for an LDX file | a folder `<name>.mapping-tmp/` with `capture.tbc`, `capture.tbc.json` (always JSON), `capture_map.tbc`; then `<file>.ldx.bak`, deleted at the end | the Map window |
  | the backup of the metadata before a tool writes them in place | `<metadata>.bup` (`ld-process-vbi`), `<metadata>.vbup` (`ld-process-vits`) | the tools |
- **what does not follow**:
  1. **A second run refuses to start**: `NoBackupOption::backUp` copies the metadata to `.bup` with `QFile::copy`, which fails when the file is there: `ld-process-vbi` run twice on a capture stops
     with "Unable to back-up input metadata file - back-up already exists?" (exit 1) unless the person deletes the `.bup` or gives `-n`. Run here on a copy of `test-data/ntsc/issue176`.
  2. **The metadata of the other format are left stale**: when a capture has both `<name>.tbc.db` and `<name>.tbc.json` and is mapped in place, only the one of the format that was read is replaced
     (the code says so: "the metadata of the other format, if there is some, stays"). The other one then describes the capture before the mapping, beside the mapped video: a tool or a person
     that reads it (`--meta json`, decode-orc on the JSON, `ld-analyse` with the JSON) gets the wrong frames.
  3. **`.bak` is not a backup**: the files set aside are deleted once the mapping is in place: nothing can be undone, while the name says the opposite. The tools use `.bup` / `.vbup` for real
     backups, which are kept.
  4. **Three ways to name a derived file**: a suffix added (`.tbc.db`, `.bup`), a suffix replaced (`.pcm`, `.meta`), a part inserted (`_map`, `_audio_0`, `.mapping-tmp`); `_map` is the only link
     between a capture and its mapping, so a renamed file loses it.
- **decode-orc**: no input file is renamed, replaced or backed up: a mapping is a parameter of the project (S-01), and files are written only by sinks, to the path the person gives. The Y/C pair is
  `<name>.tbcy` / `<name>.tbcc`.
- **proposal**: (1) at once, and low risk: a backup that exists is replaced (or numbered), not a reason to stop; one suffix for the backups of all tools. (2) a mapping in place replaces, or
  removes, the metadata of the other format too (or refuses while both are there). (3) the files set aside kept until the person says (or called what they are). (4) goes away with S-01: a mapping
  kept as metadata needs no `_map`, `.mapping-tmp` or `.bak` file. **risk**: low for (1) to (3), each a change of a file name or of a file that is left, so asked first.

## Workflow 2 - completing and converting the metadata

### S-02 - The SQLite metadata are written slowly, and all of them each time
- **kind**: suboptimal. **status**: done (2026-10-23); decided (2026-10-22): faster write AND only the tables that changed; confirmed (measured).
- **where**: `library/tbc/lddecodemetadata.cpp` (`writeSqlite`), `sqliteio.cpp` (`SqliteWriter::write*`).
- **what**: each row is written by its own `QSqlQuery` with a `prepare` of its own (`writeFieldDropouts` and the others), and `ld-process-vbi` / `ld-process-vits` write **every table of every
  field** to change one or two of them. Measured on a synthetic capture: 20 000 frames with 30 dropouts a field, 6.9 s to write the SQLite (0.2 s for the JSON); 10 000 frames with 100 a
  field, 10 s; with 300 a field, 30 s. The read of the same file is now 0.5 s.
- **effect**: a run of `ld-process-vbi` on a capture with many dropouts spends more time writing the metadata than computing them; the same for each tool in the chain and for the backup.
- **proposal**: prepared statements kept for the whole write; a write that only replaces the tables that changed (the VBI and the VITS tables for the two tools). **risk**: low for the
  first (same file), medium for the second (the update must give the same file as a full write).

### S-03 - Writing the metadata of a vhs-decode capture says it is ld-decode's
- **kind**: illogical (a wrong result). **status**: done (2026-10-23); decided (2026-10-22): keep the value that was read; confirmed (read).
- **where**: `lddecodemetadata.cpp` (`writeSqlite`), twice: `"ld-decode", // TODO: make decoder configurable`.
- **what**: the `decoder` column of the `capture` table (`CHECK (decoder IN ('ld-decode','vhs-decode'))`) is not kept: every tool that writes the metadata of a capture sets it to
  `ld-decode`, in place or in a new file.
- **effect**: a capture made by vhs-decode and run through `ld-process-vbi`, `ld-dropout-correct`, `ld-disc-stacker` or `ld-discmap` says afterwards that ld-decode made it. A reader that
  chooses by the decoder (VHS captures have no VITS, other levels...) is told the wrong thing.
- **proposal**: keep the value that was read (`VideoParameters` has no field for it: add one), write `ld-decode` only for metadata that did not come from a file. **risk**: low (schema
  and files are the same; the value is the one that was there).

### S-04 - Dropouts that are no longer there stay in the file when the metadata are updated
- **kind**: illogical (a wrong result). **status**: done (2026-10-23); decided (2026-10-22): delete a field's dropouts before writing them, with a test that reproduces it first; to verify (read; needs a run).
- **where**: `SqliteWriter::writeFieldDropouts` (`INSERT OR REPLACE`, nothing deletes), `writeSqlite` when the file exists.
- **what**: when the file is updated in place, the dropouts of a field are inserted over those of the file; those that the new metadata no longer hold are not removed. No tool of the suite
  removes dropouts in place today (the tools that change them write a new file), so it would show only with a new tool or a library user.
- **proposal**: delete the rows of a field before writing its dropouts, in the same transaction. **risk**: low.

### S-05 - `ld-process-vits` reads the whole field to measure its first 22 lines
- **kind**: suboptimal. **status**: done (2026-10-23); decided (2026-10-22): ask for lines 1 to 22; confirmed (read); to measure.
- **where**: `ld-process-vits/main.cpp` (the `FieldPool` is given no lines), `library/tbc/vitsmetrics.h` (`VITS_START_FIELD_LINE = 1`, `VITS_END_FIELD_LINE = 22`);
  `ld-process-vbi` already asks for its lines (6 to 22).
- **effect**: the whole TBC is read (8 % of it is used): on a disc that is most of the time of the run, and the reason it is slow to start on a disk that is not fast.
- **proposal**: ask for lines 1 to 22 (`FieldPool` can); the result is the same. **risk**: low (compare the metadata before and after on the captures of `test-data`).

## Workflow 1 - a capture comes in

### S-06 - A CVBS capture without its companion is measured through the whole file at each opening
- **kind**: suboptimal. **status**: done (2026-10-23); decided (2026-10-22): keep the measured IDs in the companion (checked against the size and date of the data file); confirmed (read, measured in part).
- **where**: `lddecodemetadata.cpp` (`readCvbsCore`), `cvbs.cpp` (`measureFieldPhaseIDs`).
- **what**: the CVBS core metadata have no field phase IDs, so every opening reads the first lines of every field of the data file to measure them (made 3.7 times faster and parallel in
  step 1: 0.025 ms a frame with the file in the cache, i.e. 1.4 s for an hour; from a disc that is not in the cache the time is the time to read a part of each frame).
  The result is not kept: the next tool, and the next opening of the same capture, measure again.
- **proposal** (to be decided): keep the measured IDs in the companion (`.cvbs.db`) the first time and use them after (checked against the size and the date of the data file), or measure
  later and only what is asked for. **risk**: medium (a file written beside the capture that the tool did not write before).

## Comparison with decode-orc

decode-orc (`decode-orc` repository, commit `bbc385ab`) is the project that replaces these tools upstream (its migration page says so). It solves several of the same problems in another
way. Read for this audit: `docs/technical/disc-mapping.md`, the pages of the stages (`docs/gui-user-guide/stages/`), the CLI guide, and in the code `orc/core` (the DAG executor, the
observation store and scheduler, `buffered_file_io.h`), `orc/plugins/stages/frame_map`, `tbc_source`, `cvbs_source`.

| Problem | decode-orc | ld-decode-tools today | Entry |
|---|---|---|---|
| Steps of a decode | One graph (a `.orcprj` project, or `--source/--filters/--sink` on the command line): sources, transforms, sinks. Frames are rendered **when a sink or a preview asks for them**; transforms are lookup tables or per-frame operations; **only sinks write files** | One program per step, each reading a whole capture and writing a whole new one (`ld-discmap`, `ld-disc-stacker`, `ld-dropout-correct`), then `ld-chroma-decoder`. A `.stack` session is already read without being written (by `ld-analyse`, `ld-chroma-decoder`), and LDX streams can go through pipes | S-07 |
| Mapping a capture | `frame_map` stage: a list of ranges (`1-11,21-31,...`) applied by a lookup table, "without copying sample data"; the Disc Mapper tool only **writes the ranges** into the project | `ld-discmap` writes the mapped capture again | S-01 (the same idea, proven there) |
| Joining and aligning captures | `source_join` (a lookup table, "no sample data is copied"), `source_align` (offsets from the first common VBI frame: pad all to the earliest, or trim to the first common) | The timeline of `ld-stacker-gui` and the `.stack` sessions align by disc numbers and offsets (`StackedVideo`); nothing joins captures end to end | context only |
| Per-frame measures (VBI, SNR, burst, colour frame phase, quality) | **Observers**, run on demand on the frames that are rendered, kept in a memory-budgeted store and in a **SQLite sidecar beside the project**, keyed by a fingerprint of the node (its inputs and parameters) and by the observer version, so that a measure is reused across sessions and dropped only when what it depends on changes; a background scheduler computes what is missing, by priority (the frame on screen, then the frames near it, then the rest), and can be cancelled | Separate passes that rewrite the metadata (`ld-process-vbi`, `ld-process-vits`: S-02, S-05); the phase IDs of a CVBS capture measured at each opening (S-06); `ld-stacker-gui` measures the SNR itself and keeps it in the `.stack` session under keys of the captures (a small version of the same idea) | S-08 |
| Reading the ld-decode metadata | `sqlite3` C API, statements prepared once, a cache filled once per source | Qt SQL; the read made 3.5 times faster in step 1 (forward-only, by column number); the write still slow | S-02 |
| Choosing the best copy of a picture captured twice | Scored on the colour-burst level, the white-flag SNR and the black-level PSNR, itemised in a report | `ld-discmap`: a "quality" mixing the black PSNR, a penalty, the sync confidence and the dropouts, with formulas that look wrong | S-10 |
| Reading the picture number | Lines 17 and 18 both decoded: if they agree the number is validated; if they disagree it is discarded; if only one decodes it is kept as unconfirmed | `VbiDecoder::decode` takes line 17, then line 18 over it, with no comparison | S-09 |
| Padding gaps | A gap is padded only when the capture can account for it: at most the frames captured across the gap + 100 | `ld-discmap` pads what the numbers say | S-11 |
| Names of the files beside a capture | No input file is renamed, replaced or backed up; files are written only by sinks, to the path given | `_map`, `.mapping-tmp`, `.bak`, `.bup`, `.vbup`, metadata of the other format left stale | S-12 |
| Correcting dropouts by hand | `dropout_map` stage: hints added or removed per field, edited in a window, without touching the samples | nothing: the dropouts are what ld-decode found | a feature, not a defect: noted, not proposed |
| Writing files | `BufferedFileWriter`: a 4 MB buffer, large writes | `TargetVideo` writes field by field through `QFile` (buffered by Qt) | to measure before saying anything |

### S-07 - Every step writes a whole new capture
- **kind**: suboptimal and impractical. **status**: confirmed (read); to measure on a full chain.
- **what**: mapping, stacking and correcting the dropouts of N captures of a disc write N mapped captures, a stack and a corrected stack, each tens of gigabytes, each read again by the next
  tool. A person has to run the tools in order, with the right file names (`_map`, the output of one is the input of the next), and keep or delete the intermediate files.
- **decode-orc**: one graph, rendered on demand; only the final sink writes.
- **here already**: the `.stack` session is such a description for one step (stacking), read on demand by `ld-analyse` and `ld-chroma-decoder` (`StackedVideo`); `StackedVideo` reads a session through a table of positions (see [audit-step2-mapping.md](audit-step2-mapping.md)).
- **proposal** (to be decided): make the session the description of the chain: for each source its map (S-01) and its dropout correction, then the stack; every reader (`SourceVideo`) renders it
  on demand, and the tools that write (`ld-disc-stacker`, `ld-dropout-correct`, `ld-chroma-decoder`) become the "sinks" of a session. **risk**: high (the core of the reading of a capture);
  in steps, S-01 first.

### S-08 - Measures of the video are computed by passes that rewrite the metadata, and never cached
- **kind**: suboptimal and impractical. **status**: direction (2026-10-22, the person): push the new formats (LDX for the communication between tools and the history, `.stack` sessions) rather than a separate cache; decided together with S-01 and S-07; confirmed (read). **closed** (2026-10-23, the person): no measure cache and no measure on demand. The metadata already hold the VBI and the VITS (the stacker GUI keeps its SNR in its session, the CVBS phases in `.cvbs.phases`); the black PSNR is 1 part of 1004 in the quality that `ld-discmap` uses to choose a duplicate and the map never depends on it, so nothing is added. No code change; the doc of ld-discmap says so.
- **what**: the VBI and the VITS of a capture are measured by `ld-process-vbi` and `ld-process-vits`, which read the whole capture and write the whole metadata; the tools that need them
  (`ld-discmap` for its quality, `ld-analyse`, the stacking tools for the SNR) depend on a person having run them, in the right order. What a window measures (`ld-stacker-gui`) is kept in its
  session only. The phase IDs of a CVBS capture are measured at each opening (S-06).
- **decode-orc**: observers on demand, a persistent cache keyed by what the measure depends on and by the version of the observer, and a scheduler that fills it in the background.
- **proposal** (to be decided): a measure cache in the library (a SQLite sidecar beside the capture, or a track of an LDX file), keyed by the identity of the video file and the version of the
  measure, used by the tools and the windows; `ld-process-vbi` and `ld-process-vits` kept to write the measures into the metadata for the tools of ld-decode. **risk**: medium to high.

### S-09 - The picture number is not checked between the two VBI lines that carry it
- **kind**: illogical (validity). **status**: confirmed (read: `library/tbc/vbidecoder.cpp`, `decode`, "IEC 60857-1986 - 10.1.3").
- **what**: line 17 is decoded, then line 18 is decoded over it: when the two disagree (a bit error on one of them) the value of line 18 is kept, and nothing says that it was doubtful. IEC
  60857 and 60856 carry the number twice for that reason; decode-orc keeps it only when the two agree, and flags it when only one decodes.
- **effect**: a wrong picture number reaches `ld-discmap` (a frame put in the wrong place, a gap padded) and the stacking tools (a frame lined up with the wrong one).
- **proposal**: a confidence with the number (both agree, one only, they disagree: dropped); the users of the number decide what to do with an unconfirmed one. **risk**: medium (it can change
  which frames are mapped: to compare on captures with VBI errors).

### S-10 - The "quality" with which `ld-discmap` chooses between duplicates is computed with wrong formulas
- **kind**: illogical (validity). **status**: done (2026-10-23) (read: `ld-discmap/discmap.cpp:301-337`).
- **what**: "Convert logarithmic to linear" is written `pow(bsnr / 20, 10)`, that is (bsnr/20)^10, not 10^(bsnr/20); the share of dropouts divides their length by `fieldHeight * 2 + fieldWidth`
  (a sum, not the number of samples of a frame), so a frame loses 1 % for about 1 700 samples of dropout; the weights (x1000 for the dropouts, /1004) make the dropouts decide almost alone.
  It also needs the VITS metrics in the metadata (`ld-process-vits` run before): without them the black PSNR is 0 for every frame.
- **decode-orc**: the burst level, the white SNR and the black PSNR, each reading shown in the report.
- **proposal**: fix the two formulas, and say in the docs that the VITS metrics are used (or measure them, S-08). **risk**: medium (it changes which copy of a duplicate is kept: to compare).

### S-11 - `ld-discmap` pads any gap that the numbers show
- **kind**: illogical (validity). **status**: to verify (read: `padDiscMap` has no bound on the length of a gap).
- **what**: a misread picture number at the end of a run (S-09) makes a gap of thousands of frames, all padded with black, and the mapped capture grows by that much.
- **decode-orc**: a gap is padded only up to the frames captured across it + 100 (a player can skip forward over a few pictures, not over thousands).
- **proposal**: the same bound, with a message when a gap is refused. **risk**: medium.

## Workflows 4 and 5 - stacking, correcting, decoding to video (second batch, 2026-10-22)

The documented route for several captures of a disc (`docs/How-to-guides/Working-with-multiple-discs.md`) is: `ld-process-vbi` on each capture; `ld-discmap` on each, which writes a mapped copy; **look at each copy in `ld-analyse`**
("disc-mapping is highly automated guess-work"); `ld-disc-stacker` on the copies; `ld-process-vits` on the result ("the SNR metadata will be incorrect"); optionally `ld-dropout-correct`; then `ld-chroma-decoder | ffmpeg`.
Seven steps, five of them writing a whole capture or the whole metadata, and a visual check in the middle. S-01, S-07 and S-08 are about that; the entries below are what else was found on the way.

### S-13 - The stack and the corrected capture come out without their audio, EFM and AC3
- **kind**: impractical (and the metadata then say something false). **status**: partly done (2026-10-23); confirmed (run: the outputs of `ld-disc-stacker` and `ld-dropout-correct` on `test-data` are a `.tbc` and a `.tbc.db`, no `.pcm`, though every input has one).
- **where**: `ld-disc-stacker/stackingpool.cpp`, `ld-dropout-correct/correctorpool.cpp` (no audio code at all; `ld-discmap` has it).
- **what**: the audio file of the source is not copied, mixed or linked. The metadata of the output still carry the `audioSamples` (and `efmTValues`, `diskLoc`) of the first source. A person who wants audio with the stacked video copies the `.pcm`
  of one source by hand and renames it.
- **decode-orc**: the Stacker has `audio_stacking` (`Disabled`: the audio of the source with the fewest dropouts, `Mean`, `Median`) and `efm_stacking` (`Disabled` by default: they measured that no combination beat the best single source, 21 695 sectors
  against 21 577 for the best combining mode), and every sink carries the audio channel pairs with the video.
- **proposal** (to be decided): the stack and the correction write the audio of the reference source (or of the source with the fewest dropouts) beside the video, as `ld-discmap` does, with an option for `Mean`/`Median`; the EFM and the AC3 of that source go
  with them in an LDX output. **risk**: low for the copy; medium for mixing (decode-orc's own numbers say not to mix EFM).

### S-14 - The SNR in the metadata of a stack is the first source's, and a whole pass is needed to put it right
- **kind**: illogical and impractical. **status**: done (2026-10-22) (read: `ld-disc-stacker/stackingpool.cpp`, `replaceFieldMetaData`: the metadata of a stacked field are those of the first source, only a padded field takes another source's; the how-to says to run `ld-process-vits` on the stack).
- **effect**: `ld-analyse`, the quality of `ld-discmap` if it is run on the stack, and any export show the SNR of one source as the SNR of the stack, until `ld-process-vits` has read the whole stack again.
- **here already**: `ld-stacker-gui` measures the SNR of the stacked fields as it makes them (`SnrAverage` on the first 24 lines, the code of step 1); the stacker has those fields in memory when it writes them.
- **proposal**: `ld-disc-stacker` (and `ld-dropout-correct`, whose output is a new capture too) measures the VITS metrics of each field it writes and puts them in the metadata, or at least clears the ones it cannot vouch for. **risk**: low (the values are the ones `ld-process-vits` would write; compare).

### S-15 - Dropout correction: a fixed over-correction, no limit on how far a line is taken from
- **kind**: impractical and illogical. **status**: done (2026-10-22) (read: `ld-dropout-correct/dropoutcorrect.cpp`).
- **what**: `--overcorrect` widens every dropout by a fixed 24 samples; the search for a replacement line goes on, line by line, as long as the lines it finds overlap a dropout, with no limit (`findPotentialReplacementLine`), so a line from far away can be used.
  There is no way to see what was corrected.
- **decode-orc**: the same method (including the matching of the chroma phase of PAL), with `overcorrect_extension` 0 to 48 samples, `max_replacement_distance` 1 to 50 lines (10 by default), and `highlight_corrections` that fills the corrected regions with white to show them.
- **proposal**: `--overcorrect` stays and takes an optional number of samples; `--max-distance <lines>` (default: as today, so the same output); `--highlight` for a diagnostic output. **risk**: low (new options, defaults unchanged).

### S-16 - Making a video file needs a hand-written `ffmpeg` command that the tool could give
- **kind**: impractical. **status**: confirmed (read: `docs/How-to-guides/Creating-video-from-PAL-decodes.md`: commands of about ten options each, `-pix_fmt`, `-color_primaries`, `-top 1`, `setdar=4/3`, `-flags +ilme+ildct`, the audio as `-f s16le -ar 44.1k`).
- **what**: what these options say is in the metadata of the capture (video system, widescreen, field order, the sample rate of the audio). The person has to know and type it, for every capture, for PAL and for NTSC (two pages), and a wrong flag gives a video that
  looks right and is not (the wrong field order, the wrong aspect).
- **decode-orc**: the `video_sink` stage writes the video file with the audio muxed in, choosing the settings from the metadata.
- **proposal**: `ld-chroma-decoder` (or a small `ld-export-video`) prints, with `--ffmpeg-command`, the command line for the capture it was given, and the how-to pages say to use it; no change to the output of `ld-chroma-decoder`. **risk**: low.

### S-17 - Pages that say other things than the tool (how-to pages)
- **kind**: inconsistent (documentation). **status**: done (2026-10-23) (read).
- **what**: `Creating-video-from-PAL-decodes.md` gives `--luma-nr` as "in dB (default 1.0)" and `--chroma-nr` in dB (the tool: IRE, default 0.0, corrected in step 1 in the tool page and the help only); other how-to pages repeat old options. The how-to on multiple discs describes
  a diffDOD that "rewrites the dropout metadata for all sources in a single process" and a tool "ld-diffdod" that is not in this suite.
- **proposal**: read each how-to against the real `--help` and correct it (the method of M-04, which was done for the tool pages only). **risk**: none.


## Windows, robustness and the notes of step 1 (third batch, 2026-10-22)

### S-18 - The graphs of `ld-analyse` have the lag that the stacker had, and grow with the length of the capture
- **kind**: suboptimal. **status**: done (2026-10-22) (measured on `PlotWidget` alone, offscreen: four curves, 900 x 300 pixels, a marker moved and the widget painted).
- **where**: `ld-analyse/plotwidget.cpp` (`PlotSeries::updatePath`: one `lineTo` for every point of the curve, no reduction; the item is painted whole at each repaint).
- **what**: each move of the marker costs 5.6 ms with 1 000 frames, 59 ms with 10 000, **275 ms with 94 620** (the capture of the person's friend); the first paint is 0.4 s. The graphs are the SNR and dropout graphs of a capture, which `ld-analyse` has open
  while the person moves through the frames.
- **here already**: the same fault was in `ld-stacker-gui` (1.4 s a picture), fixed in step 2 by drawing the curves once on a layer, with at most four points for each column of pixels.
- **proposal**: the same reduction in `PlotSeries` (the points that are drawn, not all), keeping the data whole for the value under the cursor. **risk**: low (look to compare; the data are the same).

### S-19 - Results that are not right, found in step 1 and kept (the "possible wrong results" of the plan)
- **kind**: illogical (validity). **status**: confirmed by reading for each (references: the notes of [audit-refactor-plan.md](audit-refactor-plan.md)).
- **what** (each is a small separate piece of work, with its test):
  1. `ld-export-metadata/closedcaptions.cpp:49` uses 29.97 for the time of a caption; the rate is 30000/1001 (A-04).
  2. In the smart-neighbour mode of `StackerCore` the centre is unsigned, so `centre - threshold` wraps when the centre is under the threshold (B-05).
  3. The dropouts read from SQLite come back in the reverse of the order of the file, those read from JSON in the order of the file (E-02).
  4. `ld-json-converter` writes the blanking level the library works out for an NTSC JSON that has none (it wrote the black level before) (E-03).
  5. A CVBS sample goes through the 10-bit word of an LDX track: `s16` (15 bits) and `tpg21` lose precision; to check against the specification of the track (F-03).
  6. `ld-discmap` corrects the NTSC CLV "offset frames"; the stacking tools and the sessions do not (A-02).
  7. The two SNR graphs of `ld-analyse` clamp at 20 dB and 14 dB and draw with other pens (C-03, J-05); the library default PAL filter is `palColourFilter` and both windows start on `transform2DFilter` (H-03).
  8. The help of `ld-ac3-decode` says its output is a stream of QPSK symbols (it is the AC3 stream) (M-04).
- **proposal**: one decision for each (fix, keep, document); 1, 2 and 8 are plain fixes.

### S-20 - A bad file can make a tool stop, hang or read beyond a vector
- **kind**: illogical (robustness). **status**: confirmed by reading; to reproduce.
- **what**:
  1. `LdDecodeMetaData::getField*` / `updateField*` log an out-of-range field number and then index the vector anyway (undefined behaviour) (E-05).
  2. `LdDecodeMetaData::getFieldNumber` looks for the next field with `isFirstField` one field at a time, to the end of the file if there is none, and logs an error **at each call**: a metadata file whose `isFirstField` is wrong everywhere makes
     each of the 95 000 frames of the capture scan to the end, which is quadratic and prints an error for each.
  3. `SourceAudio::open` stops the program (`qFatal`) when the `.pcm` beside a capture is missing or empty (F-02).
  4. `ld-process-ac3` checks the log level with `assert`, which a release build removes (N-11).
  5. `ld-discmap` and the pools index `vbiData[...]` and fields on the strength of the metadata, with checks that are not all there (to read: the metadata of a capture cut short is handled for the frames past its end, not for a hole).
- **proposal**: return an error, once, with the reason; no `qFatal` in a library; a limit on the search of (2). **risk**: low to medium (a tool that stopped hard now stops with a message).

### S-21 - Behaviour that nothing checks
- **kind**: inconsistent (tests). **status**: confirmed.
- **what**: library code with no test (`sourcevideo`, `targetvideo`, `metadataoptions`, `navigation`, `jsonio`, `efmchain`, `ldxstream`... M-07); the Python functional tests (not run here, and the `assert` fault of M-08 not looked for in them); the windows are checked by a
  build and by hand only (the step 1 changes in the status bar, rates and `NoNumber` were never seen on the screen). Step 2 added tests for what it changed (`testsqliteread`, `testsnrplot`, the phase IDs in `testcvbs`).
- **proposal**: a test for each piece of the work list before it is changed; a list of what to try by hand for the windows (it can be given with each change). The functional tests to be run once on a machine that has Python and ffmpeg.

- **S-21 begun** (2026-10-23): `testvideoio` writes video with `TargetVideo` and reads it back with `SourceVideo` (TBC whole and by lines, backwards reads, `hasField`, a missing file, a file cut inside a field, CVBS frame round trip and the dropped half frame). It found a defect: in lenient mode (`ld-analyse`), a **whole field** past the end of a TBC file, or cut short, was given as one blank line (`blankField(0, 0)`) instead of a blank field; fixed in `SourceVideo::getVideoField`. Still without a test of their own: `metadataoptions`, `navigation`, `jsonio`, `efmchain`, `ldxstream`, the LDX source/output through `SourceVideo`/`TargetVideo`.

### Not read in detail (and why)
The EFM chain (`ld-process-efm`, `efm-decoder-*`: the choice between them was deferred in step 1, I-01, I-03); `ld-process-ac3` (two programs on a pipe, not reviewed beyond its options); the exports (`ld-export-metadata`, `ld-export-decode-metadata`); `ld-container`'s twelve commands
(the container is specified and tested). None showed a point on the first reading; they come back if the person wants them looked at.


## Carried over

The "Notes for step 2" of [audit-refactor-plan.md](audit-refactor-plan.md) are to be read in the same way, one by one, and moved here when they are looked at.

## Still to read

Nothing planned: the first pass over the workflows is finished (see "Not read in detail" above). What comes next is the choice, entry by entry, with the person.

## Group 4 - quick correctives (2026-10-22)

Chosen by the person: S-14, S-15, S-17, S-18 and, of S-19, the cases 1 (to decide), 2 and 8.

- **S-14 done**: `ld-disc-stacker` and `ld-dropout-correct` measure the VITS metrics (`computeVitsMetrics`, the code of `ld-process-vits`) of each field they write and put them in the metadata (a padded field of the stacker keeps them too). Checked on `ve-snw-cut` (NTSC): the SNR values written by the stacker, and by the corrector, are identical to the ones `ld-process-vits` writes when run on the output afterwards.
- **S-15 done**: `--overcorrect-extension`, `--max-distance`, `--highlight` in `ld-dropout-correct` (defaults unchanged: same output), tool page and README updated.
- **S-17 done**: the how-to pages (IRE of the noise reduction, diffDOD) and the READMEs of `ld-dropout-correct` (`--length`), `ld-process-vbi` and `ld-process-vits` (options that do not exist) corrected. **Then (2026-10-23)**: the README of `ld-process-vits` lost the output and Python examples it described (the tool writes the metadata only; it now points to `ld-analyse` and `ld-export-metadata --vits-csv`), and the README of `ld-lds-converter` lost its sample-rate and metadata sections (the tool has no such options). The other names the scan flagged are not errors: the `--first_active_*` underscore forms are accepted aliases, `--AC3` is an ld-decode flag, `--mapped-from` an `ld-container` option, `--meta` belongs to the processing tools, the rest are ffmpeg options.
- **S-18 done**: `PlotSeries::updatePath` draws at most four points for each column of pixels (first, lowest, highest, last), the data stay whole; test `testplotwidget` (few points kept whole, many points reduced, extremes kept, repaint time).
- **S-19.2 done** (smart-neighbour wrap in `StackerCore`, expected hash of mode 3 changed and a hand-checked case added), **S-19.8 done** (`ld-ac3-decode` help).
- **S-10 done** (2026-10-23): `frameQualityPercent` (in `frame.cpp`, tested in `testframe`) has the formulas: 10^((dB - 43) / 20) for the black PSNR, and the dropouts divided by the samples of the two fields (width x height x 2). The weights are kept (dropouts 1000 of 1004). The mapped output of four captures of `test-data` is identical before and after (they have no repeated frame, so no choice depends on it); the effect on a capture with repeats has not been compared. The page of the tool says now that the VITS metrics are used.
- **S-19.1 done** (2026-10-23, the person chose drop-frame for NTSC and PAL-M, 25 frames for PAL): the SCC times of `ld-export-metadata --closed-captions` are drop-frame timecode (`HH:MM:SS;FF`) for the systems at 30000/1001, so the captions keep step with the video (a count of 30 frames a second would run 3.6 s an hour ahead); the output of the NTSC captions changes. Test `testclosedcaptions`; tool page updated. The declaration in `closedcaptions.h` (one argument) did not match the function and was corrected.

## Decisions of 2026-10-23 (the person)

- **S-12**: direction, not a list of patches: the legacy behaviour (`.bak`, `.mapping-tmp`, `_map`, `.bup`) is kept behind a **`--legacy`** mode, and the way that is favoured is a **transcoding to LDX** (the history and the maps in the file, no side files). To design with S-01 / S-08; nothing changed yet.
- **S-13**: the audio, EFM and AC3 go with the video **in LDX** (the stack and the corrected capture written as LDX carry the tracks of the reference source); the `.tbc` output stays as it is, legacy. To design with S-12; nothing changed yet.
- **S-16**: **dropped** (no `--ffmpeg-command`; the how-to pages keep their hand-written commands).
- **S-09 / S-11**: inside the building of the map (see audit-step2-mapping.md 10.5).
- **S-20**: **yes**, one point at a time, each with a test that reproduces it.
- **S-20 in progress (2026-10-23)**, done with a test each: (1) `LdDecodeMetaData::fieldAt` gives an empty field (and says so) for a number outside the file instead of indexing the vector; (2) `getFieldNumber` looks at most 8 fields ahead for a first field, says so once (not for each frame) and no longer reads before the start or after the end; both in `testmetadata` (`testBadFieldNumbers`); (3) `SourceAudio::open` returns false with a message when the `.pcm` is missing or empty (`testaudio`), and `ld-discmap` says "Cannot open source audio file" and now **exits with 1** (it ignored the result of `saveDiscMap` and returned 0 after a failed write); (4) `ld-ac3-decode` and `ld-ac3-demodulate` check the log level and the three files with a real test and exit 2 / 1 with a message (they used `assert`). (5) `ld-discmap` refuses a capture whose metadata have a frame without two fields (a hole, no first field near), with a message and exit 1; the mapped output of 4 `test-data` captures is unchanged. The pools of the stacking tools were not looked at. **(5, the pools, 2026-10-23)**: `LdDecodeMetaData::firstFrameWithoutFields()` (test in `testmetadata`) tells the first frame that has no two fields; `ld-disc-stacker` and `ld-dropout-correct` check each source with it before they begin and stop with a message (exit false) instead of working with a field number of -1. The other reads of the pools go through `getField`, which gives an empty field outside the file (1). All 40 unit tests pass.
- **S-02, S-03, S-04, S-05 done** (2026-10-23): the statements of `SqliteWriter` are prepared once and bound by place; `LdDecodeMetaData::write(file, tables)` writes only the tables a tool changed when it writes over the file it read (`ld-process-vbi`: field record, VBI, VITC, closed captions; `ld-process-vits`: VITS), and the rows of the tables that are written are taken away first, so a dropout or a VBI that the metadata no longer have does not stay (S-04); the `decoder` of the capture is kept (`VideoParameters::decoder`, S-03); `ld-process-vits` reads lines 1 to 22 of each field only (S-05). Measured with `SQLITE_BENCH=100000` in `testsqliteread` (100 000 fields, 3 dropouts a field on average): a new file 3.4 s to 1.0 s, the file again in place 3.7 s to 1.25 s, the VITS table alone 0.095 s. Outputs compared on 5 captures of `test-data`: `ld-process-vits` gives the same metadata as before; `ld-process-vbi` over the file gives the same metadata as to another file (a full write). Tests: `testsqliteread` (decoder, rows taken away, partial write).
- **S-19 decided and done (2026-10-23)**: (3) the dropouts read from SQLite come back in the order of the file (by start), as from JSON: `testsqliteread` changed; the video written by `ld-dropout-correct` (with and without `-o`) and `ld-disc-stacker --no-map` on 5 captures of `test-data` is byte for byte the same as before, only the order of the dropouts in the metadata read back from SQLite differs; (4) kept: the blanking level of a NTSC JSON that has none is the one the library works out (said in `converters.md`); (5) **no defect**: the three other CVBS encodings (`U16`, `TPG21`, `S16`) carry 10-bit values scaled up, so the 10-bit word of an LDX track loses nothing and the specification's "byte for byte" is right; (6) said in `ld-discmap.md` (the stacking tools line sources up by position in a mapped capture), and the rule of the jumps goes into the conversion of numbers of the map; (1) the SCC times and (2), (8) were done earlier.
- **S-19.7 decided, not done**: (a) the two SNR graphs of `ld-analyse` get the same limits and style, **ideally one component shared with `ld-stacker-gui`**; (b) the person proposes a **"decoding parameters" object in the LDX file**, holding the decoder settings of a capture, used as the defaults by `ld-analyse` and `ld-chroma-decoder` when present (not by `ld-stacker-gui`, which has several sources); when absent, the default filter follows the source (2D for vhs-decode captures, 3D for LaserDisc), the same default in `ld-chroma-decoder` as in `ld-analyse`, and the log says which decoder was used. This touches the LDX specification and the default output of `ld-chroma-decoder`: to be designed and asked before it is built.
- **S-09, S-11 done** (2026-10-23, in the building of the map): S-09: `VbiDecoder::Vbi::picNoUnconfirmed` says that a CAV picture number was read from one line only or that the two lines disagreed (`picNo` is what it was: line 18 wins), `ld-discmap` gives such frames the basis `u` in the map (a CLV time code is counted as measured); `ld-disc-stacker --measured-only` leaves out the frames whose basis is `u`, `s` or `g` (the pulldown frames stay). S-11: a gap of the numbers is padded only up to the frames of the capture between the two frames plus `--gap-allowance` (100); a gap that is refused is said, counted and not padded (checked on a capture with the numbers shifted by 5 000 after frame 14: 29 frames in the map instead of 5 029; with `--gap-allowance 10000` it pads as before). Tests: `testvbidecoder`, `testmappedvideo` (measured only). The padded output of the 4 captures of `test-data` is unchanged.
- **S-06 done** (2026-10-23): the phase IDs measured from the colour burst of a CVBS capture without a companion are kept in `<file>.cvbs.phases` (a header with the size and date of the data file and the number of fields, then a byte for each field) and used while the data file is as it was (`Cvbs::readPhaseCache` / `writePhaseCache`, test in `testcvbs`). It is a file of its own, not the companion (a companion would take the place of the core metadata and could go stale). On `decode_pal_1vpp.meta` (250 MB): the first opening 0.12 s, the next 0.05 s. A folder that cannot be written to is no reason to stop.
- **S-19.7 (a) done** (2026-10-23): the white SNR and black PSNR curves have the same colours and width (2) in `ld-analyse` and `ld-stacker-gui` (`gui-common/snrcolors.h`); in `ld-analyse` the white one was a thin dark gray that did not read on a dark theme, the black one was white/black at width 2. The vertical ranges stay different (white 14-42 dB, black PSNR 20-48 dB: they are what each normally is). The shared plot component itself is not done. (b), the decoding parameters in the LDX, waits for the person.
- **S-12 partly done, S-13 partly done** (2026-10-23), in the direction the person gave (LDX favoured, the old behaviour behind `--legacy`):
  - S-12: `ld-process-vbi` and `ld-process-vits` back up their metadata to `name.bup` whatever the tool, and a backup of the last run is **replaced** (it used to stop the second run, and vits used `.vbup`); `--legacy` keeps the old suffix and the refusal (test `testcommonoptions`). The `_map`, `.mapping-tmp` and `.bak` files of the Map window are gone with the map (11.1). The metadata of the other format that are left stale after an update in place are still only warned about; the backup of a metadata file made by `saveDiscMap` is `.bup` too.
  - S-13: `ld-disc-stacker` and `ld-dropout-correct` write the analogue audio of the **first source** as a `pcm` track when the output is an LDX file (`FieldAudio`: the audio of the fields written, in the order of the frames; for a source read through a map, the audio of the field of the capture each field is and silence for a padded frame), `--no-audio` leaves it out. Checked: the track extracted from the LDX output of a stack and of a correction of two copies is byte for byte the `.pcm` of the first source; the ranges through a map in `testmappedvideo`. **Not done**: the 24-bit audio of a CVBS capture, the EFM and AC3 tracks (they are a stream of the whole capture, not of fields: carrying them through a map or an offset needs a decision), a `.tbc` output (no audio, as before).
- **S-13 completed** (2026-10-23, the person asked for the other streams, noted in the history, and ideally a window and a command line version): `StreamCopy` (library) lists the streams of the LDX sources besides the video (EFM, AC3, RF, audio of 24 bits; the analogue audio is listed as written from the fields) and copies the ones chosen into the LDX output, block by block with the times they have in their source, interleaved with the fields (`TargetVideo::addLdxTrack`, `writeLdxTrack`, `ldxTimeNs`); the history of the output says what was copied from where and that it was not realigned. **CLI**: `--copy-streams none|default|1:efm,1:ac3,2:#5` on `ld-disc-stacker` and `ld-dropout-correct` (default: the EFM, AC3 and 24-bit audio of the first source; an LDX file or the LDX stream as output). **Session**: `stacking.copyStreams` (source id and track; not set: the default; empty: none), read by `ld-disc-stacker` given a session. **GUI**: the Process window has a *Streams to copy...* button (format LDX) that opens `StreamsDialog` (the streams of each source that is on, ticked to begin with as the default says) and puts the choice in the session. Checked: the EFM track of the LDX output is byte for byte the `.efm` of the source; a stream of the second source chosen by the session; the stream output carries it; tests `teststreamcopy` (candidates, choices as text, the copy interleaved with fields, read back) and the session round trip in `testmappedvideo`. Not done: realigning the streams to the frames of the stack (offsets, maps), plain `.efm`/`.ac3` files beside a `.tbc` (only the tracks of an LDX source are listed). The window was not seen.
- **Offset between the video and the copied streams recorded** (2026-10-23, the person: the stored metadata must be enough to correct any gap between video and audio, by recording the offset if there is one): each stream that `ld-disc-stacker` or `ld-dropout-correct` copies keeps the times it has in its source, and its track in the output carries `alignment` = `{"unit":"frame","segments":[[output frame, source frame, count],...]}` (`StreamCopy::alignmentOf`): for each stretch of frames of the output, the frame of the capture of that source that goes with it (0: none). It is worked out from what the tool really did: for the stacker, the frame of each source that goes with each frame of the output (offsets of a session, or VBI numbers), through the map for a source read through one (`StackingPool::computeStreamAlignments`, `ownFrameOf`); for the corrector, the first source (through its map if it has one). The history says it in words (`not shifted from the video`, `shifted ... by N frames`, `in N pieces`). Checked: a session with the second source shifted by 3 frames gives `[[1,0,3],[4,1,26]]` for its EFM and `[[1,1,29]]` for the first source's; `teststreamcopy` tests the segments and the words. The analogue audio of the output is written from the fields, so it is aligned by construction, and a capture read through a map keeps its map in the metadata (section 8.5), which holds the frame of the capture for each frame of the view. Not done: the streams of the donor sources of `ld-dropout-correct` have no alignment (they are not aligned to anything).
- **S-19.7 (b) done** (2026-10-23), as decided with the person: *Decoding parameters* (`DecodingParameters`: decoder name and numeric settings) kept with the metadata of a capture like the map (JSON member `ldxDecoding`, SQLite table `decoding`, LDX metadata blocks; `LdDecodeMetaData::saveDecodingParameters`, kept by every rewrite, not touched by the partial writes). `ld-analyse` starts the chroma decoder window from them (else its rule, one capture 3D, a stack 2D, unchanged) and has *Config > Keep the decoder settings for this capture*; the phase compensation now counts a capture made by `vhs-decode` as a tape (`DecodingParameters::isTape`). `ld-chroma-decoder` takes the same defaults (stored decoder and settings; else one capture 3D, a session or a stream 2D, phase compensation for a NTSC tape) and logs the decoder and why; **its default output changes** for a single capture (`pal2d`/`ntsc2d` to `transform3d`/`ntsc3d`), `--decoder pal2d`/`ntsc2d` keeps the old. Not done: `ld-chroma-decoder` does not write the parameters (the person chose ld-analyse only); `ld-stacker-gui` does not use them. Tests: `testsqliteread`. The window action was not seen.
- **S-19.7 (b) corrected** (2026-10-23, the person): a stack is decoded in 3D if it is of LaserDiscs and in 2D if it is of tapes (a tape format in the metadata, or made by vhs-decode); one capture stays in 3D, a stream in 2D (`DecodingParameters::prefers2D`). `ld-analyse` and `ld-chroma-decoder` use it, and the log says it (`a stack of tapes: 2D`, `a stack of LaserDiscs: 3D`). I had written that a stack is always 2D: that was the old rule of `ld-analyse`.
