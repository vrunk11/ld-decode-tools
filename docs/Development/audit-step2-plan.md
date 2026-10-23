# Audit, step 2: what is suboptimal, impractical or illogical (branch `audit-refactor`)

Step 1 (the refactor and the harmonisation) is finished and pushed: see [audit-refactor-plan.md](audit-refactor-plan.md), whose "Notes for step 2" are the first input here.
This file says how step 2 is done. The findings are in [audit-step2-findings.md](audit-step2-findings.md).

## Goal

Go through what the tools do, **as a person who uses them for a job**, and write down everything that

- **costs more than it needs to** (suboptimal): time, disk, memory, a file read or written in full for a change that is small;
- **asks more than it needs to** (impractical): steps, files, knowledge or options that a person has to carry for a result that could come by itself;
- **does not follow** (illogical): a behaviour that contradicts its name, its page, its specification or what the tool next to it does; a result that is wrong;
- **is not needed** (irrelevant): a feature or a tool that nothing uses any more or that another one covers.

The example that started it: mapping a capture with `ld-discmap` writes the whole TBC (and its audio) again, when what is new is only the order and the numbering of the frames,
which the metadata could hold.

Step 2 finds and proposes. A change that touches a file format, the metadata schema, a command line option or the output of a tool is **asked first** (AGENTS.md section 10), as in step 1.

## Method

1. **Survey by workflow.** The jobs of a person, in the order a capture goes through them, each read end to end in the code (not only searched), with the commands, the files that
   are read and written, and what is kept between two steps:
   1. a capture comes in (a TBC with its metadata, a CVBS file, an LDX file, a pipe) and is opened (`SourceVideo`, `LdDecodeMetaData`, `LdxCapture`);
   2. the metadata are completed (`ld-process-vbi`, `ld-process-vits`) and converted (`ld-json-converter`, `ld-sqlite-to-json`, `ld-tbc-to-cvbs`, `ld-cvbs-to-tbc`);
   3. the capture is put in order of the disc (`ld-discmap`, and the mapping inside `ld-disc-stacker`, the Map window of `ld-stacker-gui`);
   4. several captures are stacked (`ld-disc-stacker`, `.stack` sessions, `ld-stacker-gui`) and their dropouts corrected (`ld-dropout-correct`);
   5. the video is decoded to colour (`ld-chroma-decoder`) and looked at (`ld-analyse`, `ld-stacker-gui`);
   6. the audio, the EFM and the AC3 are decoded and kept (`ld-process-efm`, `efm-decoder-*`, `ld-process-ac3`, `ld-container`);
   7. the results go out (`ld-export-metadata`, `ld-export-decode-metadata`, the `.ldx` container).
   For each: what is read and written in full, what is computed again, what a person has to type or know, what is called the same and does something else.
2. **Measure before saying "slow".** A finding about cost has a number (a run on `test-data` or on a synthetic capture, as for the metadata of an hour of video in step 1); one that
   has none is `to verify`.
3. **Entries**, in the findings file, with the same fields as in step 1 (id `S-01`..., kind, where, what and the proof, effect, proposal, risk, status), and for each: *what it would
   change for a person*, *what it would break* (formats, options, files that other tools or ld-decode / decode-orc read) and *a way to check it*.
4. **Compare with decode-orc**, which replaces these tools upstream and solves several of the same problems in another way (a graph rendered on demand, mappings as lookup tables,
   measures cached by provenance): for each finding, what decode-orc does and whether it can be taken over. The table is in the findings file.
5. **Triage together**: one entry at a time, the person decides (keep, document, change, drop). Then the work list, in an order that puts the safe and the shared first.
6. **Doing the points**, one at a time, with the rules of the plan of step 1: characterisation test first, `build.bat` and `ctest -L unit`, before and after outputs compared, docs and
   specifications updated, nothing committed unless asked in that message. For a point about speed, the number before and after goes in the log.

## Carried over from step 1

The "Notes for step 2" of the plan (possible wrong results, relevance, tests and checks) are entries of this audit too: they are copied into the findings file as they are looked at,
under the same names (A-02, B-05, E-02...).

## Order of the survey

Workflows 1 to 4 first (they are where the captures are largest and where the examples are), then 5 and 6, then 7. The windows are read with the jobs they are used for.

## Result (2026-10-23)

Step 2 is finished: the workflows were read, 21 entries (S-01 to S-21) were decided one by one with the person and done, and the notes of step 1 were gone through. All of it is committed on `audit-refactor` (ten commits after `6175dd2`, the last one `5592d9c`), 46 unit tests pass.

**What changed for a person**
- A capture is put in order of the disc **without being written again**: the map is kept in the session or in the metadata of the capture (JSON `ldxMap`, SQLite table `disc_map`, LDX), and every tool that reads a capture follows it (`--ignore-map` reads it as it is; `ld-discmap --legacy` writes the copy as it did). Six basis letters say where each number of the map came from (measured, unconfirmed, sequence, pulldown, guessed, padded), a gap is padded only as far as the capture can account for.
- The metadata are written faster and only the tables that changed (100 000 fields: 3.4 s to 1.0 s; in place 3.7 s to 1.25 s; VITS alone 0.095 s); `ld-process-vits` reads 22 lines of a field; the phase IDs of a CVBS capture are kept; the windows read the metadata and open sources faster and draw the graphs once.
- A stack or a corrected capture in an LDX file carries the analogue audio, the other streams chosen (EFM, AC3, RF, 24-bit audio) with their own times and the **alignment** with the video, and the history says what was copied; a window and `--copy-streams` choose the streams.
- The decoder and its settings are kept with the metadata of a capture (`ldxDecoding` / table `decoding`); `ld-analyse` keeps and uses them, `ld-chroma-decoder` takes them as its default (3D for one capture or a stack of LaserDiscs, 2D for a stack of tapes).
- Results that were wrong are right: the SNR of a stack is measured, the quality that chooses between duplicates, the SCC times, the smart-neighbour wrap, the order of the dropouts, a picture number that two lines disagree on, the audio of an LDX output (it was one frame late).
- Robustness: an out-of-range field is no longer read, a missing `.pcm` no longer stops a tool, a capture with a hole in its fields is refused with a message, the JSON reader takes what other programs write, cancelling a stack no longer stops the program or leaves a half-written file, and the windows no longer stop on a build for an AVX CPU (the cause was in the build, not in a window).

**How it was checked**: before and after outputs compared on `test-data` (byte for byte where nothing was meant to change), numbers for every point about cost, a test that reproduces each defect before it is fixed, and, at the end, the windows run for real and read from their image. 46 unit tests (16 on `main` at the start of the audit).

**What is left, said**
- Not run here: the functional tests (`ctest -L functional`: Python and ffmpeg; read, no fault of M-08, two small remarks in the findings), the Windows/MSVC and macOS builds (only MinGW was built).
- Not read: the encoders `ntscencoder` and `palencoder` (only make test signals for `scripts/test-chroma`); the EFM chain (out of the scope, decided). Read in step 2: `ld-ac3-demodulate` and `ld-ac3-decode` (a crash on any input of a few hundred kilobytes, fixed) and the Transform PAL filters (nothing wrong; one thing noted).
- Not seen on the screen in `ld-analyse`: the play and seek panels over the picture, the View menu; the Style tab of the audio panel was opened but not read closely. (Seen: the main window, all the analysis windows, the Config windows, the audio panel tabs Output, Import and EFM, the Remote control, the Playback menu.)
- Without a test of its own: `navigation` and `efmchain`.
- Built but not done: the shared plot component of the two SNR graphs (the colours are shared), the realignment of copied streams (the alignment is recorded, nothing is moved), the donor sources of `ld-dropout-correct` have no alignment, plain `.efm` / `.ac3` beside a `.tbc` are not offered as streams to copy, and the session as the description of the whole chain (S-07, not planned).
- Two decisions kept as they were: the metadata of the other format are only warned about (S-12), and the cosmetic names of the windows listed in `glossary.md`.