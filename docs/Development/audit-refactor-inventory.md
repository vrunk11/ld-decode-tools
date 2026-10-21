# Audit and refactor inventory (branch `audit-refactor`)

What overlaps, what is redundant and what does the same job in different ways, with proof. The way it is made, and what the fields mean, is in
[audit-refactor-plan.md](audit-refactor-plan.md) (phase 1). Status is `to verify` until the places were read and compared.

**Pass 1 is finished (see the end).** Every entry below is `confirmed`: both places were read, or a count or a run showed it.

Size of the code (lines of C++ and headers, from `git ls-files`, 2026-10-21, before any change on the branch):

| Part | Lines | Files |
|---|---|---|
| library (`tbc` 17905, `ldx` 3092, `filter` 1208) | 22205 | 92 |
| ld-analyse | 20592 | 80 |
| efm-decoder | 14726 | 108 |
| ld-process-efm | 7826 | 35 |
| ld-stacker-gui | 7532 | 18 |
| ld-chroma-decoder | 7203 | 36 |
| ld-discmap | 2784 | 8 |
| ld-export-decode-metadata | 2768 | 9 |
| ld-container | 1804 | 1 |
| ld-process-ac3 | 1735 | 14 |
| ld-dropout-correct | 1632 | 5 |
| ld-disc-stacker | 1468 | 5 |
| ld-export-metadata | 869 | 9 |
| ld-json-converter | 840 | 3 |
| gui-common | 831 | 4 |
| ld-process-vbi | 538 | 5 |
| ld-process-vits | 523 | 5 |
| ld-lds-converter | 508 | 3 |
| ld-tbc-to-cvbs | 361 | 1 |
| ld-cvbs-to-tbc | 238 | 1 |
| ld-sqlite-to-json | 136 | 1 |

## A. The VBI, frame numbers and timecodes

### A-01 - The same scan of the VBI of a source, copied in two tools
- **kind**: duplicate. **status**: done (2026-10-21) (for the part read: the loop that decodes the VBI of each frame, counts CAV and CLV, and takes the minimum and
  maximum frame; only the log prefix differs).
- **where**: `src/ld-disc-stacker/stackingpool.cpp` (`StackingPool::setMinAndMaxVbiFrames`, about lines 395-445 and after) and
  `src/ld-dropout-correct/correctorpool.cpp` (`CorrectorPool::setMinAndMaxVbiFrames`, about lines 355-400 and after).
- **effect**: a fix or a change (what counts as a valid CAV number, how CLV is read) has to be made twice.
- **proposal**: one function in the library (for example next to `VbiDecoder`) that gives, for a capture, the disc type, the first and last frame number and the
  counts; both tools call it. **risk**: low if the function is pinned down by a test first.
- **done**: `DiscNumbering::scanRange` (+ `sequentialToDisc`, `discToSequential`) in `library/tbc/discnumbering.{h,cpp}`, a verbatim move of the loop; `StackingPool` and `CorrectorPool` call it and keep their own log lines. Test `testdiscnumbering` (CAV with a gap, a 0 and a frame with no number; CLV NTSC and PAL; both kinds; none; the conversions). Checked: `ld-disc-stacker` and `ld-dropout-correct` on 5 pairs of `test-data` (3 CAV, 1 CLV, 1 with no VBI) give files, exit codes and messages identical to those before. This is the first piece of the numbering module that A-02 to A-05 will fill.

### A-02 - Four more ways to say "which disc number is this frame"
- **kind**: divergent. **status**: done (2026-10-21) (read `discmap.cpp:135-185`, `stackedvideo.cpp:53-80`, `stackingpool.cpp setMinAndMaxVbiFrames`, `lddecodemetadata.cpp:2137-2170`).
- **found**: the arithmetic is the same (a CLV time is `((h*60+m)*60+s)*fps + picture`, fps 25 or 30), written in `convertClvTimecodeToFrameNumber` and again in `discOrigin` (which has `framesPerSecond` of its own). What is not the same is the *validity*: `discmap` and `stackingpool` take a CAV number when `picNo > 0` and a CLV time when the four parts are not -1; `discOrigin` takes `picNo != -1` (so 0 counts) and for a CLV time tests only the hour and the picture, not the minute and the second (a minute of -1 gives a number 60 too low). `discmap` and `stackingpool` choose CAV or CLV for a whole capture by count; `discOrigin` takes whichever a frame has, by vote over 600 frames. `discmap` also corrects the NTSC CLV offset frames (`isNtscAmendment2ClvFrameNumber`); the others do not.
- **decided (with the person)**: one rule, and the numbers of the disc can be **0 or negative** (the frames of a lead-in come before picture 1). A picture number of 0 is a number (-1 is "none"); a CLV time counts only with its four parts; "no number" is `DiscNumbering::NoNumber` and not -1, so that an origin of -1 or less is a real origin and a capture that begins with unnumbered frames is no longer moved to 0.
- **done**: `DiscNumbering::cavNumber`, `clvNumber`, `frameNumber` (+ `NoNumber`) in the library; `scanRange`, `StackedVideo::discOrigin` and `alignedPositions`, the counts and the CLV numbers of `ld-discmap` (`discmap.cpp`) and the 9 places of `ld-stacker-gui` that tested -1 or `>= 0` use them. `discOrigin` no longer takes a time code with a missing minute or second. Tests: `testdiscnumbering` (zero, negative origin, corrupt frame, CLV PAL and NTSC, none) and `teststackproject` (negative and zero origins in `alignedPositions`). Checked on `test-data`: `ld-discmap` output (8 captures), `ld-disc-stacker` and `ld-dropout-correct` (5 pairs) and sessions read by `ld-chroma-decoder` and `ld-disc-stacker` (3 sessions, with offsets of 3, -2 and 5 frames) give files identical to before. Not done here: the NTSC CLV offset frames of `ld-discmap` stay in `ld-discmap`; the text of a number (A-05) and the frame rate (A-04). Specification (section 13) says the origin can be 0 or negative.
- **where**: `src/ld-discmap/discmap.cpp` (about lines 142-176: frame number from CAV picture or CLV time, per frame);
  `src/library/tbc/stackedvideo.cpp` (`StackedVideo::discOrigin`: CAV picture or CLV time in frames, by vote over the first 600 frames);
  `src/library/tbc/navigation.cpp` (`NavigationInfo`: chapters from the VBI); and A-01 above.
- **what to compare**: what each counts as a valid number (`picNo > 0` against `picNo != -1`), how a corrupt number is handled (the vote, a sort, a jump), what
  they return for a capture with no number, and the frame rate used for CLV (A-04).
- **effect**: two tools can number the same capture differently.
- **proposal**: a single library description of "the numbering of a capture" that the four read. **risk**: medium (ld-discmap's numbering decides what it writes).

### A-03 - A second copy of the CLV conversions
- **kind**: duplicate. **status**: done (2026-10-21). Found on the way: the two functions of `ExportMetaData` were never called (dead), so the copy was deleted, not replaced. The rest of `ExportMetaData` is E-04.
- **where**: `src/ld-export-decode-metadata/exportmetadata.cpp` (about lines 1145-1193: `convertClvTimecodeToFrameNumber`, `convertFrameNumberToClvTimecode`, with
  25 and 30 written out) and the same two functions of `LdDecodeMetaData`.
- **proposal**: one. **risk**: low.

### A-04 - The frame rate written out again and again
- **kind**: duplicate. **status**: done (2026-10-21), except one place (see below) (places listed; `git grep` of the forms over `src/`).
- **found**: outside ld-analyse, `ld-stacker-gui/mainwindow.cpp:2270,2389,2393,2448`, `library/tbc/stackedvideo.cpp:56` (`framesPerSecond` of a timecode, 25 or 30) and `ld-export-metadata/closedcaptions.cpp:49`, which uses **29.97** (rounded) for the time of a caption where ld-analyse uses `30000.0 / 1001.0`: three values for the NTSC rate (30, 29.97, 29.97002997). The 29.97 is a divergence to look at in step 2 (a drift of 0.0001 % on the time of a caption; about 0.4 s per 100 hours).
- **done**: `frameRateNumerator`, `frameRateDenominator`, `videoFrameRate` (the time of the video: 25, 30000/1001) and `timecodeFramesPerSecond` (what a time code counts: 25, 30) in `lddecodemetadata.h`; used by `convertClvTimecodeToFrameNumber` and `convertFrameNumberToClvTimecode` (six written-out 25/30 gone), by the 8 places and the frame-rate fraction of `ld-analyse/mainwindow.cpp` and by the 4 of `ld-stacker-gui/mainwindow.cpp`. Test: `testmetadata` (`testFrameRateAndTimecodes`: the rates, the round figures, PAL, NTSC and PAL-M, to a number and back, a missing part). **Left as it is**: `ld-export-metadata/closedcaptions.cpp:49` keeps 29.97, since using the exact rate changes the times written in the files it makes; it is a point for step 2.
- **where**: `src/ld-analyse/mainwindow.cpp` (`PAL ? 25.0 : 30000.0 / 1001.0` at about lines 594, 876, 3196, 3347, 3583, 3796, 3844, 4491, and `25 : 30` at
  3876, `25 : 30000` at 505); `src/ld-export-decode-metadata/exportmetadata.cpp` (1157-1193); `src/library/tbc/stackedvideo.cpp`;
  `src/ld-stacker-gui/mainwindow.cpp` (`framesPerSecond`, several places, added on this branch's parent).
- **what to compare**: CLV timecodes count 25 and 30 frames to the second (integer), whereas the time of the video goes by 25 and 30000/1001. The two uses must stay
  apart, but each should be one named function or constant.
- **proposal**: `framesPerSecond(system)` (timecode) and `frameRate(system)` (time) in the library. **risk**: low.

### A-05 - The text of a CLV or CAV number, made in seven places
- **kind**: duplicate and inconsistent. **status**: done (2026-10-21).
- **where**: `src/ld-analyse/vbidialog.cpp:96-108`, `src/ld-analyse/mainwindow.cpp:1378-1386` (status bar: "CLV time code: hh:mm:ss", "CAV picture number: 00000", zero-padded to 5),
  `mainwindow.cpp:4405-4406` (the remote's headline through `clockText`, the picture padded to 5), `src/ld-analyse/remotelogic.cpp:155` (`h:mm:ss`, the hour **not** padded) and `clockText` in the
  same file, `src/ld-export-metadata/csv.cpp` (about lines 146-152), `src/ld-stacker-gui/mainwindow.cpp` (`MainWindow::discFormat`).
- **what**: in ld-analyse alone four spellings of one time: `hh:mm:ss` (status bar), `hh:mm:ss.ff` with the picture of the second (VBI dialog), `h:mm:ss` (clock), and a picture number that is
  padded to 5 in the status bar and not at all in the VBI dialog (`QString::number(vbi.picNo)`). The seconds of a time (`clvHr * 3600 + clvMin * 60 + clvSec`) are also worked out
  three times (`tbcsource.cpp:1498,1503`, `mainwindow.cpp:4405`).
  `ld-export-metadata/csv.cpp:140-160` makes a fifth text, `hh:mm:ss.ff`, with `xx:xx:` and `xx.xx` for the parts that are missing, and `none` for none, where the VBI dialog just leaves
  them out. The same function has two branches that do the same (`if (vbi.type != unknownDiscType) {cav; clv} else {cav; clv; unknown}`, and a second `userCode.isEmpty()` inside the
  `else` of the first one, which can never be true): dead code (see O).
- **proposal**: one formatter and one "seconds of a CLV time" in the library (see A-02); where a text differs on purpose (a clock for the position in the video against the time on the disc),
  the two are named apart. A change of a text shown is asked. **risk**: low.
- **decided (with the person)**: the format of `ld-stacker-gui` everywhere (a CLV time `1:23:45.06`: hours as they are, the other parts on two digits, `xx` for a part not known), a CAV picture with no zeros (`123`); **except the remote control**, which keeps its own clock and its 5-digit picture (`1:23:45`, `00123`), and so does the "Disc time" column of the chapter list (it has seconds only, no picture).
- **done**: `DiscNumbering::cavText`, `clvText(h, m, s, picture)` and `clvText(frames, system)`; used by the status bar and the VBI dialog of `ld-analyse`, by `ld-export-metadata` (CSV) and by the disc label of `ld-stacker-gui`. Test `testdiscnumbering` (`testText`). Visible changes: status bar of ld-analyse `01:23:45` -> `0:19:14.29` (gains the picture) and `00123` -> `123`; VBI dialog `00:19:14.29` -> `0:19:14.29`; **CSV of ld-export-metadata: the hour of the CLV time code loses its leading zero (`00:19:14.29` -> `0:19:14.29`)**, checked on 5 captures: nothing else in the file changes. Also removed: a branch of the CSV code that could never run. The seconds of a CLV time are still added up in 3 places of ld-analyse (`tbcsource.cpp` twice, the remote), left to the point on the numbering of the chapter list.

## B. Stacking, mapping and correction across several sources

### B-01 - The stacking core is called from three places, each gathering its own data
- **kind**: duplicate. **status**: done for two of the three (2026-10-21) (the three call sites were read).
- **where**: `src/library/tbc/stackercore.cpp` is shared. It is called by `src/ld-disc-stacker/stacker.cpp:68-69` (with `stackingpool.cpp` finding the fields),
  `src/library/tbc/stackedvideo.cpp:378` (`StackedVideo::stackField`, for ld-analyse and ld-chroma-decoder) and `src/ld-stacker-gui/framebackend.cpp:614-617`
  (`FrameBackend::stackFrame`). Each one decides alone which fields are usable (pad flag, field available in the file, same size), reads them, picks the mode
  (`StackerCore::autoMode` at `stackedvideo.cpp:283` and `framebackend.cpp:607`) and builds the arguments.
- **effect**: a rule for what a stack may use (for example the check that a field is in the file, which was missing in the GUI and made it stop) has to be made in each.
- **proposal**: one library function "stack a frame from these sources" taking readers of fields and their metadata, used by the three. **risk**: medium (a
  stack must give the same bytes as before: it is checked with `test-data`).
- **done**: `StackFrame` in `library/tbc/stackframe.{h,cpp}`: `ownFrame` (the frame of a source that falls on a frame of the timeline; it was in 3 places), `isPadded`, `read` (which sources can give the frame: not padded, the fields in the metadata and in the file, the size of the first, the lines read all there; one field or both, the first lines only), `stack` (the call to `StackerCore`, the smart threshold times 256) and `repairFieldPhases` (B-02). Used by `StackedVideo` (sessions: ld-analyse, ld-chroma-decoder), by `FrameBackend::stackFrame` (the preview of ld-stacker-gui) and, for `ownFrame` and the automatic mode (`StackerCore::autoMode`, which its own `if` chain repeated), by `ld-disc-stacker`. Test `teststackframe` (frames, padded, size, file shorter than the metadata, one field or both, the first lines, the parameters, the phases). Checked: sessions read by `ld-chroma-decoder` and `ld-disc-stacker` (3 sessions) and the 5 pairs of the CLI give files identical to before. **Not done: the CLI keeps its own gathering of fields** (`StackingPool::getInputFrame`: the mapping by the VBI, the integrity check, the fields read for all the sources in one go), since it is a different job (VBI mapping, threads, integrity); its selection of sources (`getAvailableSourcesForFrame`) is B-03. **Not checked: the preview of ld-stacker-gui** (no way to run it here); it calls the same two functions as the sessions do, and `teststackframe` checks that both fields stacked together are those stacked one at a time. A difference of behaviour: the video parameters given to the stacking are those of the first source (as ld-disc-stacker and the sessions do) and not those of the first usable source (the preview did): only for a frame where the first source is not usable and the sources have different parameters.

### B-02 - The repair of the field phases of a stack, in two places
- **kind**: duplicate. **status**: done (2026-10-21) (read both). The loop is the same text. **They differ in one thing: the library repairs only NTSC (`stackedvideo.cpp:208`), `ld-disc-stacker` repairs whatever the system (`stackingpool.cpp:587`) with `PHASE_COUNT = 4`**; a PAL field has eight phases, so what it does to PAL is a question for step 2 (not tested).
- **decided (with the person)**: correct it: 4 phases for NTSC and PAL-M, 8 for PAL. **done**: `StackFrame::repairFieldPhases`, used by `ld-disc-stacker` (both calls) and by the sessions (which only repaired NTSC). Measured on `test-data/pal/jason-testpattern`: the phases of the input are 7, 8, 1, 2, 3, 4, 5, 6; the stack made by the old tool had 3, 4, 1, 2, 3, 4, 1, 2; now 7, 8, 1, 2, 3, 4, 5, 6. Checked: the `.tbc` of the 5 pairs and the NTSC `.tbc.db` are identical to before; **only the `.tbc.db` of PAL stacks changed** (2 pairs of the CLI, the PAL sessions), and the images decoded from the PAL sessions are identical. Test `teststackframe` (PAL 8, NTSC and PAL-M 4, nothing but padded fields). Docs: `docs/Tools/ld-disc-stacker.md`, section 13 of the specification.
- **where**: `src/ld-disc-stacker/stackingpool.cpp` (about lines 595-660: first field that is not padded as the pivot, then the sequence of 4 phases) and
  `src/library/tbc/stackedvideo.cpp:210-226` (`readMetadata`, the same pivot and phase loop).
- **proposal**: one function on `LdDecodeMetaData` or in the stack code. **risk**: low if the result is compared.

### B-03 - Three tools map the sources by their VBI, each its own way
- **kind**: redundant, and divergent. **status**: checked (2026-10-21): the rules of validity are one now (A-02), the scan of the range is one (A-01); what is left is a difference of job, not a copy. Run: `ld-disc-stacker` on 4 pairs of `test-data` stacked from the captures as they are (`--no-map`) and from the same captures after `ld-discmap` give the same `.tbc` byte for byte. The captures of `test-data` have no gap, so the case that matters (a gap, a repeated frame, a capture that `ld-discmap` cannot number) was not run. Proposed at triage: `drop` as a refactor, and write in step 2 that the in-memory mapping and `ld-discmap` were not compared on a capture with gaps.
- **where**: `ld-disc-stacker` (maps in memory unless `--no-map`, see `stackingpool.cpp`), `ld-dropout-correct` (`correctorpool.cpp`, the same scan, see A-01), `ld-discmap`
  (maps the frames of one capture and writes it), and the GUI (aligns by `StackedVideo::discOrigin`, see A-02). The documentation says a capture should be mapped first.
- **to find out**: whether the in-memory mapping of ld-disc-stacker gives the same order as ld-discmap does for the same capture, and what happens to captures that
  ld-discmap cannot number (it keeps them, the in-memory one may refuse them).
- **proposal**: to be decided with the person: either the in-memory mapping is the library's one that ld-discmap uses too, or it is documented as a different
  thing. **risk**: high (it changes what is stacked).

### B-04 - A session is read by three code paths
- **kind**: divergent. **status**: done (2026-10-21), and corrected
- **found**: in the library itself the session is loaded twice by two functions (`StackedVideo::readMetadata` and `StackedVideo::open`, each its own `StackProject` and its own walk over the sources that are on). `ld-disc-stacker` turns the session into command line variables by hand (mode, `smartThreshold` times 256, `diffDod` inverted into `noDiffDod`, passthrough, integrity, reverse) and aligns by the offsets only; `ld-stacker-gui` turns the same settings into the arguments of `ld-disc-stacker` (G-01). Three translations of `session.stacking`, one of which (the 256 and the inversion) is easy to get wrong.
- **corrected**: `ld-stacker-gui` does **not** translate the session into arguments: it writes the `.stack` and gives that file to `ld-disc-stacker` (`processdialog.cpp:282`, `402`). So the session is turned into settings in two places only: `ld-disc-stacker/main.cpp` and the library (`StackedVideo`), which now share `StackFrame::stack` (B-01). **done**: the three copies of "find the metadata of a source, read it, set the field order" inside `stackedvideo.cpp` are one (`readSourceMetadata`); `readMetadata` and `open` still each walk the sources (one reads the metadata only, the other opens the videos), which is left. Checked: sessions identical to before.
- **where**: `src/ld-disc-stacker/main.cpp` (about lines 240-275: the session becomes a list of inputs and offsets), `src/library/tbc/stackedvideo.cpp` (reads the same
  session as a virtual video), `src/ld-stacker-gui/mainwindow.cpp` (its own project, `projectForProcessing`).
- **what to compare**: alignment (the CLI aligns to the first source with offsets only, the other two by the disc numbers; written in the specification as allowed, but a
  person sees two stacks of one session), mapping (`followMap`), the format of the sources.
- **proposal**: ld-disc-stacker takes the alignment from the library too. **risk**: high (changes the length of what it writes), so asked about.

### B-05 - Repeats inside one file
- **kind**: duplicate. **status**: done for `stackercore.cpp` (2026-10-21); the two others go with their domains (`f1toaudio.cpp` with I, `sqliteio.cpp` with E) (a search for runs of 7 identical lines inside each `.cpp`; only three files have more than a dozen).
- **where**: `library/tbc/stackercore.cpp` (30 lines repeat: `:485-502` is a copy of `:431`, `:466,523,542` of `:447`, and the smart-mean and neighbour modes repeat the selection of samples), `ld-process-efm/Decoders/f1toaudio.cpp`
  (`:370-392` is a copy of `:312`), `library/tbc/sqliteio.cpp` (`:599-611` a copy of `:555`).
- **proposal**: small local helpers; `stackercore.cpp` is the heart of the stack and has no unit test of its own: characterisation tests first (the output of each mode on a small fixed input). **risk**: medium for `stackercore`, low for the others.
- **done**: `teststackercore` (new; characterisation test first): for each of the 5 modes, 1 to 5 sources with spikes and dropouts, with and without the differential dropout detection and the pass-through, two thresholds: one number per mode (a hash of every output sample and dropout), taken from the code as it was, plus a few results checked by hand (median, mean, smart mean, `autoMode`). Then `smartMean` (the loop that was written 2 times), `neighbourValue`, `neighbourResults` and `closestToNeighbours` (written 2 times each, in the smart neighbour and neighbour modes) were extracted; the 5 numbers are unchanged, the CLI outputs on 5 pairs are unchanged. `smartMean` keeps the arithmetic of its caller's type: in the smart neighbour mode the centre is unsigned, so `centre - threshold` wraps for a centre under the threshold (noted for step 2).

## C. Dropouts

### C-01 - The total length of the dropouts of a frame, added up in six places
- **kind**: duplicate. **status**: done (2026-10-21).
- **where**: `src/ld-analyse/tbcsource.cpp:1391,1399`; `src/ld-discmap/discmap.cpp:332,336`; `src/ld-stacker-gui/framebackend.cpp:147,533,654` (three times, added
  on this branch's parent: `addSource`, `measureStackFrame`, `render`).
- **proposal**: `DropOuts::totalLength()` (and `isWholeField(width, height)`, now `FrameBackend::isWholeDropout`) in `src/library/tbc/dropouts.h`. **risk**: low.
- **done**: `DropOuts::totalLength`, `visibleLength` (the part in the picture, which `ld-analyse` wrote out twice, once for each field) and `coversWholeField` in the library; used by `ld-analyse` (`generateData`), `ld-discmap` and the 3 places and `isWholeDropout` of `ld-stacker-gui`. Test `testdropouts` (new: `dropouts` had none; M-07). A dropout that ends before it starts now counts 0 everywhere (only `ld-stacker-gui` did). Checked: `ld-discmap` output on 4 captures, the CLI and the sessions: identical.

### C-02 - Two ways to draw the dropouts on a picture
- **kind**: duplicate and inconsistent. **status**: done (2026-10-21), except the lookup by pixel for ld-analyse (read `tbcsource.cpp:451-500`); the ld-stacker-gui side was read earlier.
- **found in ld-analyse**: the colours are fixed in the code (first field red, second blue, `Qt::red` / `Qt::blue`), whereas ld-stacker-gui takes them from the theme (light and dark per
  field, a residual colour that can be customised). The frame rows are worked out in place for each view (`FRAME_VIEW`: `(fieldLine-1)*2` / `fieldLine*2-1`; `SPLIT_VIEW`: other
  formulas). A third place looks a dropout up by pixel: `TbcSource::getScanLineData` (about line 758) loops over every dropout for every pixel of the line, with `<= endx`
  (the end included), where the drawing draws `startx..endx`; whether the end is included or not was not compared with the library.
- **where**: `src/ld-analyse/tbcsource.cpp` (about lines 451-480: highlights the dropouts of the field on the picture) and `src/ld-stacker-gui/framebackend.cpp`
  (`drawDropouts`, `drawSourceDropouts`).
- **to compare**: rows used (the lines of a field in a frame), colours, the rule for a field that is a dropout as a whole (only the GUI skips it).
- **proposal**: a library function that draws a `DropOuts` on a frame image, with a colour, used by both. **risk**: low to medium (ld-analyse's picture must not change).
- **done**: `DropoutDrawing::draw` in `gui-common` (`dropoutdrawing.{h,cpp}`: the row of a line in four layouts: frame, split, field, field stretched), used by `ld-analyse` (`TbcSource::getImage`, which kept its red and blue) and by `ld-stacker-gui` (`FrameBackend::drawDropouts`, which keeps its themed colours, lighter for the first field and darker for the second). `gui-common` now links the library. Test `testdropoutdrawing`: the result is compared pixel by pixel with a `QPainter` drawing written as `ld-analyse` had it, for the 4 layouts, both fields and 3 colours, on dropouts that cross the edges and lie outside the picture. **Not done**: `TbcSource::getScanLineData` (the oscilloscope) still looks a dropout up by pixel on its own. The colours of the two programs are still not the same (red/blue and the theme); to be decided if wanted.

### C-03 - What a whole-field dropout is, known by one tool only
- **kind**: inconsistent. **status**: done (2026-10-21).
- **where**: `ld-discmap` writes a dropout over the whole of each field for a frame it cannot number (`discmap.cpp` about line 876); only the GUI knows to leave such a
  frame out of its graphs and picture; `ld-analyse` and `ld-dropout-correct` treat it as an ordinary (huge) dropout.
- **proposal**: say it in the metadata (a flag) or in the library (C-01), and decide what each tool does with it. This touches the metadata, so it is asked about.
  **risk**: medium.
- **decided (with the person)**: in the library (`DropOuts::coversWholeField`), no flag in the metadata; `ld-analyse` does as `ld-stacker-gui`: such a frame is not highlighted and counts 0 in the two dropout graphs. `ld-dropout-correct` and `ld-disc-stacker` unchanged. **done** in `tbcsource.cpp` and `docs/Tools/ld-analyse.md`. **Not run**: ld-analyse was built but its window was not seen.

## D. SNR and VITS

### D-01 - The SNR of a frame from its two fields, in five places
- **status**: done (2026-10-21), see the end of the entry.
- (a fifth, `TbcSource::analyseLiveFrame`, `ld-analyse/tbcsource.cpp:1957-2005`, for a live input: the same rule with `isfinite`, and it also decodes the VBI of the field: `decodeFieldVbi` is
  called from there, `ld-process-vbi` and its own `vbilinedecoder.cpp`.)
- **kind**: duplicate. **status**: confirmed (read `tbcsource.cpp:1453-1481`, `filesnr.cpp:43-56`, `livesnr.cpp:48-67`). The three in ld-analyse apply the same rule (the average of the fields
  whose value is above 0; `filesnr` and `livesnr` also test `isfinite`, `tbcsource` does not; no field means NaN in all three, by `nan("")` or by 0/0). `filesnr.cpp` and `livesnr.cpp` hold the
  same two lines word for word. Four with ld-stacker-gui. The core is shared; the dialogs then each clamp the value their own way (see J-05).
- **done**: `SnrAverage` in `library/tbc/vitsmetrics.{h,cpp}`: `add` for the values of a field, for the metrics of a field, for a field measured (`VitsRawMeasurements` and the levels of the video) or for its test signal lines; `white()` / `black()` (NaN if no field has a value) and `hasWhite()` / `hasBlack()`. Used by the 5 places of `ld-analyse` (`generateData`, `analyseLiveFrame`, `FileSnr`, `LiveSnr`) and the 6 of `ld-stacker-gui` (`frameSnr` of the metadata of a source, the frame of a source and of the stack that are shown, the points of the stack and of a source), whose `frameSnr` and `measureField` are gone. Test `testvitsmetrics` (new): none, one field, two, 0 / negative / NaN / infinite, white without black, from metrics, from measurements = the average of `applyVitsLevels`. A difference: `generateData` did not test `isfinite` (an infinite value was added; now left out). **Not run**: the two GUIs and the graphs of `ld-analyse` (built only).
- **where**: core in `src/library/tbc/vitsmetrics.cpp` (`measureVitsRaw`, `applyVitsLevels`, used by all). The average of the two fields into a value for the frame is
  in `src/ld-analyse/filesnr.cpp` (about lines 43-107), `src/ld-analyse/livesnr.cpp` (about lines 45-133) and `src/ld-stacker-gui/framebackend.cpp`
  (`measureField`, `measureSourceFrame`, `frameSnr`, `measureStackFrame`); `src/ld-process-vits/vitsanalyser.cpp` writes the per-field values into metadata with
  `computeVitsMetrics`.
- **what to compare**: the rule for a field with no value (finite and above 0 in ld-stacker-gui; how in ld-analyse), rounding, the lines read.
- **proposal**: a library function "SNR of a frame from two fields" used by the three. **risk**: low to medium (the graphs of ld-analyse must not change).

## E. Metadata

### E-01 - `jsonio` is copied in ld-export-decode-metadata
- **kind**: duplicate. **status**: done (2026-10-21) (the two files differ by 2 lines out of 479).
- **where**: `src/ld-export-decode-metadata/jsonio.cpp`, `jsonio.h` and `src/library/tbc/jsonio.cpp`, `jsonio.h`.
- **proposal**: delete the copy and use the library's. **risk**: low.
- **done**: `jsonio.{h,cpp}` of `ld-export-decode-metadata` deleted (the two differed by a comment), the tool uses the library's. Checked: its export of the 5 captures of `test-data` is identical to before.

### E-02 - The SQLite schema is written twice
- **kind**: duplicate. **status**: done (2026-10-21), decided with the person
- **where**: `src/ld-json-converter/jsonconverter.cpp` (`createSchema`, about lines 231-420, 8 `CREATE TABLE`) and `src/library/tbc/sqliteio.cpp` (a schema text with 8
  `CREATE TABLE IF NOT EXISTS`).
- **effect**: a column added to one is missing in the other; the output of ld-json-converter would then differ from the one of the library.
- **proposal**: one schema (the library's). **risk**: low if the files written are compared before and after.
- **found**: 4 tables of 8 differed (`drop_outs`: no primary key and another column order in the library; `vbi`, `vitc`: no NOT NULL; `vits_metrics`: `w_snr` and `b_psnr` the other way round). The converter's schema was the one of ld-decode (checked against `test-data/*.tbc.db`), the library's was looser.
- **decided (with the person)**: the library takes ld-decode's schema. **done**: the 4 tables of `sqliteio.cpp` as ld-decode makes them (`drop_outs` with `FOREIGN KEY` then `PRIMARY KEY (capture_id, field_id, field_line, startx, endx)`); every query of the library names its columns, so reading is not touched; the writers use `INSERT OR REPLACE`, so a dropout written twice is kept once. Test `testsqliteschema` (new): a new file written by the library has, for the 8 tables, the same text of schema as `test-data/ntsc/issue176.tbc.db` that ld-decode made. A file that exists keeps its schema. The new files (converter, `.cvbs.db` companions, `ld-tbc-to-cvbs`) have the official one.

### E-03 - ld-json-converter reads with the library but writes with its own code
- **kind**: duplicate. **status**: done (2026-10-21).
- **where**: `jsonconverter.cpp:32-78` (`process`: `LdDecodeMetaData::read`, then its own `createDatabase`, `createSchema`, `insertData` with 8 `INSERT INTO`) against
  `LdDecodeMetaData::write` and `writeSqlite` (`lddecodemetadata.cpp:836-873`) which already write the same file.
- **proposal**: the converter becomes: read JSON, set the output format, write. `ld-sqlite-to-json` (136 lines) is already like that. **risk**: low.
- **done**: `JsonConverter::process` reads, reports, removes an output that is there, and writes with `LdDecodeMetaData::write` as SQLite: `createDatabase`, `createSchema` and `insertData` (about 500 lines) are gone, with a declaration of a function that was never written (`countDropouts`). Checked on 5 captures (JSON from `ld-sqlite-to-json`, to SQLite, back to JSON): the data read back are identical to before **except one value** (below).
- **a value that changed, to decide**: for a JSON without `blanking16bIre` (every NTSC file of `test-data`, made before the member existed) the old converter stored the **black** level as the blanking level (its comment: "JSON written before blanking_16b_ire existed lacks it: use black_16b_ire"); the library, as every other tool, stores the blanking level it works out when it reads the JSON (15360 for `issue176`, whose black is 7.5 IRE above). The 2 PAL captures are the same either way. See the notes of the plan.

### E-04 - Two SQLite to JSON tools, one of them with its own metadata model
- **kind**: redundant, and duplicate. **status**: partly done (2026-10-21): the copy of `jsonio` is gone (E-01) and the `VideoSystemDefaults` of the tool is in an anonymous namespace (the compiler warning about the one definition rule is gone); the second model (`ExportMetaData`) stays until the person says
- **proof by the compiler**: building both gives the warning "type `struct VideoSystemDefaults` violates the C++ One Definition Rule" (`exportmetadata.cpp:39`): the same struct is declared in the tool and in the library.
- **proof by search**: `ld-export-decode-metadata/exportmetadata.cpp` shares 182 windows of 8 lines with `library/tbc/lddecodemetadata.cpp`, `jsonio.cpp` 256 with `library/tbc/jsonio.cpp`, and `exportdropouts.{h,cpp}` are a copy of `dropouts.{h,cpp}`: the tool holds a renamed copy of the metadata model (E-01).
- **found**: `MetadataConverter::process` reads with `LdDecodeMetaData` and then copies every parameter and every field into `ExportMetaData` (`convertVideoParamters`, `convertPcmAudioParamters`,
  `convertField`: a second model of the same data, with its own enum of video systems, `ExportMetaData::parseVideoSystemName`). The program only takes `--input-sqlite`, though the library it reads
  with accepts a JSON file too (the other tools take either). The comments of `main.cpp` are those of another tool ("Remove .json extension if present, then add .db", "--input-json"), and
  its README repeats the standard options twice.
- **where**: `src/ld-sqlite-to-json` (writes the ld-decode JSON, `.tbc.json`, through the library) and `src/ld-export-decode-metadata` (2768 lines: its own
  `ExportMetaData`, `exportdropouts`, `metadataconverter`, writes an "export" JSON `.export.json` with a version field, for outside tools).
- **to find out**: whether `ExportMetaData` is a model apart from `LdDecodeMetaData` (it has its own CLV conversions, see A-03), and whether the two JSON formats need
  to be two.
- **proposal**: to be decided: keep both with the reason written, or build the export JSON on the library's model. **risk**: medium (the export format is read by other programs).
- **why it may be kept**: the README of the tool says that the SQLite format is internal and subject to change, and that the export is "a (mostly) stable, documented format" with a version field for outside tools: a model apart from `LdDecodeMetaData` is what keeps the export stable when the library changes. Proposed at triage: `keep`, with that reason in the code (the header of `exportmetadata.h`).

### E-05 - `LdDecodeMetaData` does everything
- **kind**: inconsistent (a design point). **status**: the accessors done (2026-10-21); the split by format is not done and not proposed
- **where**: `src/library/tbc/lddecodemetadata.cpp` (2256 lines): reads and writes JSON, SQLite, CVBS companions and LDX; field access; frame and field numbering;
  CLV conversion; defaults.
- **found (second look, `lddecodemetadata.cpp:1777-1940`)**: eight `getField*` and eight `updateField*` accessors (field, vits, vbi, ntsc, vitc, closed caption, dropouts, clear dropouts) repeat the same four lines
  (`fieldNumber = n - 1; if out of range -> qCritical(...); return fields[fieldNumber].x`). **The range test only logs, and then the vector is indexed anyway** (undefined behaviour for a field that does not
  exist; the same in `getField`): a validity question for step 2. The copies carry the errors of copying: `updateField` logs "updateFieldVitsMetrics()", the comment of `updateFieldVitsMetrics` says "VBI".
- **proposal**: one private `fieldAt(n)` that checks and returns the field; the sixteen accessors become one line each. Whether an out-of-range access should still only log (and what it must then return) is
  a decision (it changes behaviour, but only where it was undefined). The split by format is not a refactor to do for itself; to be looked at when the points above are done. **risk**: low for `fieldAt`, high for the split.
- **done**: private `LdDecodeMetaData::fieldAt(n, caller)`; the 8 getters and 7 updaters (field, vits, vbi, ntsc, vitc, closed caption, dropouts, clear dropouts) are one line each. It **keeps what they did**: it logs an out-of-range number and then indexes all the same (noted for step 2). The message now names the real function (`updateField` said "updateFieldVitsMetrics"). Test `testmetadata` (`testFieldAccessors`: each kind of data goes to its own field and to no other).

## F. Opening the video, and the command line

### F-01 - The same command line options defined again in each tool
- **kind**: duplicate, and inconsistent. **status**: done for -t, -r and -n (2026-10-21); the other options of N-03 to N-10 are not done.
- **where**: `-t/--threads` (same text, same `idealThreadCount()` default and the same check `< 1`) in `ld-chroma-decoder/main.cpp:234,410,437`,
  `ld-disc-stacker/main.cpp:101,222`, `ld-dropout-correct/main.cpp:91,118`, `ld-process-vbi/main.cpp:77,95`, `ld-process-vits/main.cpp:80,98`; `-r/--reverse` in four tools;
  `-n/--nobackup` in `ld-process-vbi` and `ld-process-vits`. `--meta`, `--input-metadata`, `--output-metadata`, `--debug` and `--quiet` are already shared
  (`MetadataOptions`, `logging.cpp`), which is what the rest should look like.
- **proposal**: a helper in the library next to `MetadataOptions` (`addThreadsOption`, `threadCount`...). **risk**: low (the messages and exit codes must stay).
- **done**: `ThreadsOption`, `ReverseOption`, `NoBackupOption` (with `backUp`) in `library/tbc/commonoptions.{h,cpp}`, used by `ld-chroma-decoder`, `ld-disc-stacker`, `ld-dropout-correct`, `ld-discmap`, `ld-process-vbi`, `ld-process-vits`. Test `testcommonoptions` (new). Checked: exit codes for `-t 0` and `-t abc`, the backups (`.bup` for vbi, `.vbup` for vits, a second run refused), the metadata written, and the help of the 6 tools are the same as before, **except one line of the help of `ld-chroma-decoder`**: "(default number of logical CPUs)" is now "(default is the number of logical CPUs)", as in the 4 others. The suffix of the backups is left as it was (N-02).

### F-02 - Several readers of audio
- **kind**: duplicate, or divergent. **status**: looked at again (2026-10-21): not merged, proposed `keep`
- **where**: `library/tbc/sourceaudio.cpp` (PCM beside a TBC), `audioreader.cpp`, `cvbsaudio.cpp` (24-bit WAV of CVBS), `ldxcapture.cpp` (`openAudio`, `getAudio`),
  `ldxbytetrack.cpp`, and `ld-analyse`'s own audio code.
- **found**: `AudioReader` (library; used by ld-analyse and `audiopeaks`) reads "the audio of a capture whatever it is kept as": an LDX pcm track, the `.pcm` beside a `.tbc`, the `_audio_0.wav` beside a `.cvbs`, memory, a growing file, a ring. `SourceAudio` (used only by `ld-discmap`) reads the same `.pcm` beside a `.tbc` and the LDX audio through `LdxCapture`, with its own random access; `WavReader` (`cvbsaudio.h`, used by `ld-container`, `ld-discmap` and `AudioReader`) reads the WAV of a CVBS capture. So the `.pcm` beside a `.tbc` and the audio of an LDX track are opened by two classes (`AudioReader::open` and `SourceAudio`), with two sets of tests (`testaudio` covers both).
- **to find out**: which are the same job (random access to samples of a capture) with a different source, and whether a single interface would do.
- **second look**: `SourceAudio` (used by `ld-discmap` only) reads for a job of its own: the exact samples of each field to write them with the capture that is mapped, with silence for what the file does not hold (`getAudioDataPadded`), and it stops the program when the `.pcm` is missing or empty (`qFatal`). `AudioReader` (ld-analyse) plays any track, also one that grows or arrives, and returns what it has. Putting one on the other would change the reading of the track (`AudioReader::tracksOf` lists every track of an LDX file, `SourceAudio` takes the one `openAudio` gives), and the way `ld-discmap` fails when the audio is missing. Not worth the risk for what is shared (opening a `.pcm`). Proposed at triage: `keep`, the reason in the headers. The `qFatal` of `SourceAudio::open` is a point for step 2.
- **risk**: medium.

### F-03 - Conversions of the samples of a TBC and of a CVBS file, in several places
- **kind**: duplicate. **status**: done for the 10-bit word (2026-10-21) (read the five places).
- **found**: the 10-bit word of a sample is made by `converter->toTbc(x) >> 6` (truncation) in `targetvideo.cpp:235` and `ld-container/main.cpp:427`, undone by `fromTbc(v << 6)` at `ld-container/main.cpp:1720`; and `Cvbs::SampleConverter::fromTbc` rounds in its own way (`tbcTo10Bit`, `cvbs.cpp:101`, `(tbc + 32) / 64`). The direction "stored sample to 10-bit word" is exact for `u10` and `u16`, but for `s16` (15 bits of precision) and `tpg21` it goes through the 16-bit domain and `>> 6`, so the word the LDX keeps is the 10-bit rounding of the sample (by design: the specification says a CVBS track is 10-bit; to check in step 2 that the specification says so). The helper that does the pair ("stored sample to LDX word" and back) is written three times instead of once. `ld-tbc-to-cvbs/main.cpp:34` has its own blanking level (`tbcBlankingLevel`, "as decode-orc does") beside `Cvbs::standardLevels`. The proposal below is unchanged.
- **where**: `library/tbc/cvbs.cpp` (`SampleConverter::toTbc`, `fromTbc`), `library/tbc/targetvideo.cpp:235`, `ld-container/main.cpp:427,1720`, and the level
  mapping of `ld-tbc-to-cvbs/main.cpp:34` (`tbcBlankingLevel`).
- **proposal**: only `SampleConverter` converts. **risk**: medium (checked by the existing `testcvbs`).
- **done**: `SampleConverter::toWord` and `fromWord` (the 10-bit word of an LDX track and back) used by `TargetVideo` and the 2 places of `ld-container`. Test `testcvbs` (`testWords`: the word is `toTbc >> 6` for the 4 encodings and 2 systems; a 10-bit sample goes through and back unchanged). Checked: a TBC converted to CVBS (u10 and s16), packed in an LDX file, unpacked, and the CVBS converted back to TBC give the same files as before. **Left**: `tbcBlankingLevel` of `ld-tbc-to-cvbs` (its own blanking level; step 2); the s16 loss of precision in an LDX track stays (noted).

### F-06 - The `main()` of the tools that process a capture
- **kind**: duplicate. **status**: partly done (2026-10-21); confirmed by the search (`ld-disc-stacker/main.cpp` and `ld-dropout-correct/main.cpp` share 40 runs of 8 lines; `ld-process-vbi/main.cpp` and `ld-process-vits/main.cpp` 38;
  `ld-process-ac3/decode/main.cpp` and `demodulate/main.cpp` 18; the six `efm-decoder` mains about 20 each); read for the first two pairs.
- **what**: after the options (F-01), the same steps are written in each. `ld-disc-stacker` and `ld-dropout-correct` (the multi-source pair): the 32-source limit, "none of the input filenames repeated", "none used as the output", "the output does not already exist", "the video standard matches the primary source", "TBC and metadata fields match", "the output stays in the container of the first input", the piped first input as an LDX stream. `ld-process-vbi` and `ld-process-vits` (the one-source pair): open the input metadata, work out the output, back the metadata up (N-02), start the pool, write back. The text of the messages is the same, with
  small differences ("Unable to open ld-decode metadata file" and "Unable to open TBC metadata file").
- **proposal**: a small `ToolRun`/helper class in the library for the common steps (open, check, output, backup, write back), after K-01 and N-02. **risk**: medium (it touches every tool).
- **done**: `SourceFileChecks` in `library/tbc/sourcefilechecks.{h,cpp}` (`outputIsNotAnInput`: no input is the output and the output is in the container of the first input; `inputsAreDistinct`; `outputIsNew`), used by `ld-disc-stacker` and `ld-dropout-correct` in the same order and with the same messages. Test `testsourcefilechecks` (new). Checked: the messages and exit codes of the 5 cases of each tool (output is an input, input twice, output exists, another container, all good) are the same, except that an output that exists is **1** (a file in the way) and no longer 2, as N-09 says. **Left**: the limit of 32 sources, the "at least n inputs" and the metadata opening, whose texts differ between the two; the one-source pair (`ld-process-vbi` and `ld-process-vits`) after K-01.

### F-04 - Piped input: the options and their checks, written in two programs
- **kind**: duplicate. **status**: done (2026-10-21) (read `ld-analyse/main.cpp:60-225` against `ld-chroma-decoder/main.cpp:340-620`).
- **what**: `--input-format`, `--system`, `--black-level`, `--white-level`, `--subcarrier-locked`, `--sample-encoding`, `--buffer-frames` are defined with the same names and nearly the same texts
  in both, and the same checks follow with the same messages ("Unknown input format ... - use ldx, tbc or cvbs", "Unknown sample encoding ...", "Unknown video system ...", "The black and white levels must be
  positive", "--subcarrier-locked must be yes or no, not ...", "The buffer must hold at least 1 frame", the warning that the levels do not apply to CVBS). Some places are not the same: the default
  `--buffer-frames` is 32 frames in the decoder and 5 seconds (125 or 150 frames) in the analyser; ld-analyse has `--throttle`, the decoder does not.
- **proposal**: a `PipedInputOptions` class beside `MetadataOptions` in the library: it defines the options, parses them and returns a description of the input (system, encoding, levels, locked),
  with a way to add the options that only one tool has. **risk**: low (the messages stay).
- **done**: `PipedInputOptions` in `library/tbc/pipedinputoptions.{h,cpp}`: the 6 options (`--input-format`, `--system`, `--black-level`, `--white-level`, `--subcarrier-locked`, `--sample-encoding`) and the reading of each value with its message (`format`, `videoSystem`, `sampleEncoding`, `levels`, `subcarrierLocked`). `ld-analyse` and `ld-chroma-decoder` use it and keep their own `--buffer-frames` (the defaults differ: 5 seconds against 32 frames) and `--throttle`, and the decision of when the options apply. Test `testpipedinputoptions` (new). Checked: for 9 combinations of values, good and bad, the exit code, the messages and the RGB output of `ld-chroma-decoder` and the exit code and the messages of `ld-analyse` are identical to before. **Visible: the help.** The two programs now say the same for the 6 options (the wording of `ld-chroma-decoder`: "Piped input without metadata: ..."; `ld-analyse` said "Piped input: ..."; the text of `--input-format` is the one of `ld-analyse`).

### F-05 - The suffixes of a capture, listed again in the audio search of ld-analyse
- **kind**: duplicate. **status**: partly done (2026-10-21) (`TbcFiles` has `baseName`, `isCaptureFileName` and `mappedFileName`; `sidecaraudio.cpp` computes the base name by hand, chopping `_chroma`, and keeps its own list of suffixes).
- **where**: `ld-analyse/sidecaraudio.cpp:19-23` (`notAudio`: a list of 50 suffixes, `rf` and `lds` twice) against `TbcFiles` (`library/tbc/tbcfiles.cpp`: `isCaptureFileName`, the chroma file).
- **what**: the list of the files that a capture can be beside is kept by hand. A new kind of file (as `.ldx` and `.stack` were) has to be added there too.
- **proposal**: ask `TbcFiles` (a function that says whether a name belongs to a capture or to its companions). **risk**: low.
- **done**: `SidecarAudio::find` finds the base name of a capture with `TbcFiles::baseName` and the track of an LDX file with `LdxCapture::split` (no `#` parsing of its own; `split` asks for a name that ends with `.ldx`). Test: `testwaveform` (which finds the sidecars of a capture) still passes. **Left**: the list of 50 suffixes `notAudio` is still kept by hand there; a function of `TbcFiles` that names the files of a capture is a point to do if the person wants it.

## G. The LDX container

### G-01 - Programs that drive command line tools instead of calling the library
- **kind**: divergent. **status**: looked at (2026-10-21): proposed `defer` (step 2 or a later branch); confirmed (read).
- **where**: `src/ld-stacker-gui/mapdialog.cpp` and `processdialog.cpp` build command lines for `ld-container`, `ld-discmap`, `ld-process-vits` and run them with
  `QProcess`; `ld-container` itself is one file of 1804 lines (`main.cpp`) with no library behind its operations (`pack`, `add`, `remove`, `stack`, `extract`, `recompress`),
  so no other program can do them without the command.
- **effect**: the operations are not covered by the library unit tests, and a program that wants to do the same has to copy command lines.
- **proposal**: move what `ld-container` does into `src/library/ldx/` (or `tbc`) functions, and keep `main.cpp` for the command line. **risk**: medium to high (large).
- **decision proposed**: no code in this branch. Moving 1800 lines into the library is a rewrite of the tool, not a harmonisation, and `ld-container` has its own tests through the command. Recorded for triage.

### G-02 - Several ways in the library to open "an LDX capture"
- **kind**: divergent. **status**: looked at (2026-10-21): proposed `drop`; confirmed: no duplicate found (read the headers and the openers). The files are layers: `Ldx::Reader` (blocks, file or stream), `LdxStream` (the standard input as an LDX stream), `LdxCapture` (a capture by name `file.ldx#3`), `LdxFieldFilter`, `LdxByteTrack` (EFM and AC3 as a device). The only repeat is the parsing of `#N` in `ld-analyse/sidecaraudio.cpp` (F-05). Proposed at triage: `drop` (keep as it is).
- **where**: `library/ldx/ldx.cpp` (`Ldx::Reader`), `library/tbc/ldxcapture.cpp` (a capture by name `file.ldx#3`), `ldxstream.cpp` (a stream on the standard input),
  `ldxbytetrack.cpp` (EFM and AC3 tracks), `ldxfilter.cpp` (pass a stream through).
- **to find out**: what is shared (reading blocks, the index, the metadata merge) and what is repeated (for example `LdxMetadataMerger`).
- **risk**: medium.

## H. Chroma decoding and the picture of a frame

### H-01 - The chroma decoder is set up and run in three programs
- **kind**: duplicate. **status**: partly done (2026-10-21); confirmed (read the places).
- **found**: the choice "PAL or PAL-M: PalColour, otherwise Comb; then the output writer" is written four times: `tbcsource.cpp:1067` (`configureChromaDecoder`), `:1610` (load; this one also turns on phase compensation for a chroma TBC or a two-file source), `:1776` (stream), and `framebackend.cpp:339`. The choice of decoder when a frame is decoded (mono or colour, one source or luma + chroma) is made again in `TbcSource::decodeFrame` (`:1156`); `ld-chroma-decoder` makes it by name (`main.cpp:660-700`); `ld-stacker-gui` has no mono decoder. The phase compensation default (on for tape) exists in ld-analyse only.
- **where**: `ld-chroma-decoder/decoderpool.cpp` and `main.cpp`; `ld-analyse/tbcsource.cpp` (lines 1067-1076, 1610-1616, 1776-1778: three places in one file that update the
  PAL, NTSC, mono and output configuration); `ld-stacker-gui/framebackend.cpp` (`configureDecoders`, `chromaPicture`).
- **proposal**: one helper that builds the decoders from the video parameters and decodes a frame to an image; ld-analyse's three places become one. **risk**: medium.
- **done**: `ld-chroma-decoder/colourconfiguration.h` (`usesPalColour`, `updateColourConfiguration`) used by the four places (three in `ld-analyse/tbcsource.cpp`, one in `ld-stacker-gui/framebackend.cpp`); the phase compensation tweak stays at the load site. Behaviour identical, build ok, `ctest -L unit` 26/26. **Left**: decoding a frame to an image (shared with H-02).

### H-03 - Two chroma settings dialogs, with their own limits, labels and units
- **kind**: duplicate and inconsistent. **status**: done (2026-10-21): the help and page say IRE (text only); limits kept (decided); the PAL filter is "PALcolour (2D)" in both windows (the stacker-gui text); defaults and controls kept (the library default palColourFilter is overridden by both windows: step 2); confirmed (read `ld-analyse/chromadecoderconfigdialog.cpp`, `ld-stacker-gui/optionsdialogs.cpp:161-265`, `ld-chroma-decoder/main.cpp:470-495`).
- **what**: the same `PalColour::Configuration` / `Comb::Configuration` are edited by `ChromaDecoderConfigDialog` (ld-analyse: sliders, radio buttons, a "Mono" choice, the phase on a
  cubic scale, a check box that turns the luma noise reduction on) and by `ChromaConfigDialog` (ld-stacker-gui: spin boxes, combo boxes, no mono, no FFT overlay). Differences
  that are not just looks:
  - **the unit of the noise reduction**: both windows say IRE; `ld-chroma-decoder --luma-nr / --chroma-nr` and its page `docs/Tools/ld-chroma-decoder.md:191,286` say **dB**. The code
    multiplies the level by `irescale` (`comb.cpp:705,762`, `palcolour.cpp:392`, `monodecoder.cpp:95`), that is, the level is in IRE. The help and the page are wrong (a matter for step 2).
  - the limits: ld-analyse clamps the noise reduction to 0..10 (`chromadecoderconfigdialog.cpp:106-108`), ld-stacker-gui has spin boxes 0..10, the command line only refuses a negative value.
  - the defaults: the library default for PAL is `palColourFilter`; ld-analyse (`tbcsource.cpp:36`) and ld-stacker-gui (`setCurrentIndex(1)`) both start with `transform2DFilter`; the command line
    has a decoder that is chosen "automatically" (`decoder.cpp`), not compared yet.
  - the labels for the same filter ("PALcolour (2D)", "Transform PAL 2D", "1D/2D/3D" combo and the radio buttons of ld-analyse).
- **proposal**: one place for the limits, the default configuration and the unit (the library); the help text and the page corrected; the labels of the two dialogs made the same. Whether the two
  dialogs become one widget in `gui-common` is to be decided (they edit one thing, but the controls differ). **risk**: medium.

### H-04 - The luma noise coring, written three times; the Y/C sum twice
- **kind**: duplicate. **status**: done (2026-10-21); confirmed (read `comb.cpp:757-796`, `palcolour.cpp:387-423`, `monodecoder.cpp:88-130`, `ld-analyse/tbcsource.cpp:1099-1150`).
- **what**: the high-pass filter, the feed of zeros round the active area, the clip to `nr_y = yNRLevel * irescale` and the subtraction are the same algorithm in `Comb::FrameBuffer::doYNR`,
  `PalColour::doYNR` and `MonoDecoder::doYNR` (the last says "same formula in both existing routines"). They differ by the filter (`f_nr` for NTSC, `f_nrpal` for PAL; mono chooses by system) and by
  being called per frame or per line. In `TbcSource::loadInputFields` the sum of a luma and a chroma field ("remove the offset 32767, clamp to 16 bits") is written twice, once for the weave and once
  for the frame (`CHROMA_OFFSET_WEAVE` and `CHROMA_OFFSET`, same value).
- **proposal**: `coreLuma(line, filter, level)` in the decoder directory (or the library), and a function that adds a chroma field to a luma field; characterisation test on a field of `test-data`
  (the functional tests of the chroma decoder already pin the output). **risk**: low to medium.
- **done**: `ld-chroma-decoder/lumacoring.h` (`coreLumaLine`, the caller says whether the filter state carries over between lines: Comb keeps one filter for the frame, PalColour and MonoDecoder a fresh one per line) used by the three; `addChromaToLuma` in `ld-analyse/tbcsource.cpp` for the two sums. Test `testlumacoring` (new) compares with the old algorithm, bit for bit, for both filters, with and without carry-over. Build ok, `ctest -L unit` 27/27.

### H-02 - The picture of a frame (weave, fields, line doubling)
- **kind**: duplicate. **status**: looked at again (2026-10-21): not merged, proposed `keep`; confirmed (read `framebackend.cpp:217-300` and `tbcsource.cpp:1217-1345`).
- **found**: the grey picture of a frame (the 16-bit sample divided by 256, the first field on the even rows, the second on the odd ones) is made in `FrameBackend::weave` and in `TbcSource::generateQImage`; the field view and the split view (which rows) in `FrameBackend::applyView` and in the `FIELD_VIEW` / `SPLIT_VIEW` cases of `generateQImage` (the second says in a comment that it is written "as ld-analyse shows a field"). They are not the same code: ld-analyse has the "bob" (the bottom field one line lower) and the stretch option, `applyView` does not. A third is `ld-analyse`'s weave playback (`loadWeave`, `testweave`).
- **where**: `ld-stacker-gui/framebackend.cpp` (`weave`, `applyView`) and the frame image code of `ld-analyse/tbcsource.cpp` (about lines 400-500 and the field views).
- **proposal**: library image helpers. **risk**: low to medium.
- **second look**: what is really common is ten lines (a 16-bit sample divided by 256 on alternate rows of two fields). ld-analyse builds a vector of pixels that three view modes copy from (and the chroma picture comes from the same vector), stacker-gui builds an image and applies the view to it, with other rules (no bob, no stretch). Sharing the ten lines would need ld-analyse to be rebuilt round a picture, with a risk of changing what is shown, for nothing gained. Proposed at triage: `keep`. Left to step 2: ld-stacker-gui has no bob in its field view, ld-analyse has.

## I. EFM and AC3

### I-01 - Two EFM decoding stacks that share no code
- **kind**: redundant. **status**: looked at (2026-10-21): proposed `keep both, document`; confirmed that they share no code (no include between them, different files for the same stages: de-interleave, F1/F2,
  Reed-Solomon, subcode, sectors); to verify that they are equal in what they do.
- **where**: `src/ld-process-efm` (7826 lines, one program, `Decoders/` and `Datatypes/`) and `src/efm-decoder` (14726 lines, `libs/efm` and four staged tools
  plus `efm-stacker-f2` and `vfs-verifier`).
- **found (second look)**: the stages are the same list in both (`ld-process-efm/Decoders`: efmtof3frames, syncf3frames, f3tof2frames, c1circ/c2circ/c2deinterleave, f2tof1frames, f1toaudio, f1todata; `efm-decoder/libs/efm/src`: efm,
  frame, delay_lines, interleave, inverter, reedsolomon, rspc, section, sector, subcode, tvalues, audio). Both use the same Reed-Solomon headers (`ezpwd`) through their own wrappers
  (`ld-process-efm/Datatypes/sector.cpp:229,311`; `efm-decoder/libs/efm/src/reedsolomon.cpp`, `rspc.cpp`). Neither is the library's; `ld-analyse` runs either of them as a program (G-01, I-02).
- **effect**: the biggest overlap by size. The documentation presents `ld-process-efm` as the single-step tool and `efm-decoder-*` as the staged one; it does not say
  which one a person should use, or whether they give the same result.
- **proposal**: to be decided with the person (keep both and document why, or make one the core of the other). **risk**: high.
- **decision proposed**: no code. Making one stack the core of the other changes the result of a decoder (14000 lines, with the Reed-Solomon work) and cannot be checked here beyond the tests; for triage, with a sentence in the docs on which one to use.

### I-02 - The same EFM settings, a different option name in the two stacks
- **kind**: inconsistent. **status**: decided (2026-10-21): no change (the person); confirmed (options read in `ld-process-efm/main.cpp:67-93`, `efm-decoder/tools/*/src/main.cpp`; the mapping is written out in
  `ld-analyse/efmdecoding.cpp:65-80`, which has to know both).
- **what**:
  | meaning | `ld-process-efm` | `efm-decoder-*` |
  |---|---|---|
  | no time stamps in the EFM | `-t`, `--time` (the help says "no time-stamp information": the name says the opposite) | `--no-timecodes` (f2, d24, audio, data) |
  | conceal bad audio | `-c --conceal` (default), `-s --silence`, `-g --pass-through` | `--no-audio-concealment` (off) |
  | pad the start to the disc time | `-p`, `--pad` | `--zero-pad` |
  | write a WAV header | not a choice | `--no-wav-header` (off) |
  | data instead of audio | `-b`, `--data` | a different program (`efm-decoder-data`) |
  The debug options are `--debug-efmtof3frames`... in one and `--show-*-debug` in the other.
- **effect**: `ld-analyse` carries two translations of its own four checkboxes; a person has to learn both.
- **proposal**: one set of names (the new one for the staged tools, the old kept as aliases in `ld-process-efm`, with `--time` renamed because its name is wrong). A change of
  option names: to be asked. **risk**: low (aliases).

### I-03 - Sources copied inside the EFM code
- **kind**: duplicate. **status**: deferred (2026-10-21, the person): no code; confirmed (a search for runs of 8 identical lines across `src/`, listed in [audit-refactor-clones.md](audit-refactor-clones.md), then read).
- **what**:
  - the tables and the steps of the CD sector (the CRC32 of the EDC, the 2352-byte scrambling table, the unscrambling) are in `ld-process-efm/Datatypes/sector.h` (288 lines, tables included) and in
    `efm-decoder/tools/efm-decoder-data/src/decoders/dec_data24torawsector.cpp` (436 lines): **142 windows of 8 lines are the same**; `efm-decoder/libs/efm/src/sector.cpp` (291 lines) is a third place for the sector;
  - `reader_data24section.cpp` is copied between `efm-decoder-audio` (139 lines) and `efm-decoder-data` (148 lines), 37 lines of `diff`; `decoders.h`, `efm_processor.h` and `main.cpp` are alike in the four
    tools `efm-decoder-{f2,d24,audio,data}` (the options `--no-timecodes`, `--show-*-debug`, the call of the processor, the help text), and `writer_f2section.h`, `reader_f2section.h` are repeated again in
    `efm-stacker-f2`; the `libs/efm` library is there to hold what the tools share, but these files are in the tools' own directories.
- **proposal**: move the readers, the writers and the processor skeleton into `libs/efm`; the sector tables into one place used by both stacks (I-01). **risk**: medium (the functional tests of the EFM tools in CI are the check).
- **second look**: none of the files with the same name is identical (md5 compared): `reader_data24section` differs at the end of the stream (audio marks the section invalid when the read fails, data has `hasMoreData()`), `reader_f2section` and `writer_f2section` differ between `efm-decoder-d24`/`efm-decoder-f2` and `efm-stacker-f2`. Merging means choosing a behaviour: left to step 2 with I-01.

## J. The programs with a window

### J-01 - The View > Theme and View > Style menus, written in each program
- **kind**: duplicate. **status**: done (2026-10-21); confirmed (read; the code in ld-stacker-gui says it is "as in ld-analyse").
- **where**: `src/ld-analyse/mainwindow.cpp` (about lines 2769-2840) and `src/ld-stacker-gui/mainwindow.cpp` (about lines 2360-2440: the menu block,
  `customizeTheme`, `updateAppearance`). The theme itself is shared (`src/gui-common/`).
- **proposal**: the menus and their handlers go in `gui-common`. **risk**: low.
- **done**: `gui-common/appearancemenus.{h,cpp}` (`AppearanceMenus`: reads the stored theme, palette and style, applies them, builds View > Theme and View > Style, runs the theme editor; the program listens to `themeModeStored`, `customPaletteStored`, `styleNameStored`, `changed`, `previewed`, `customizeRequested`). ld-analyse keeps its `Configuration`, ld-stacker-gui its `QSettings`, with the same keys as before. About 250 lines less in the two windows. Test `testappearancemenus` (new). Both programs start offscreen; the menus and the theme editor to be tried by the person.

### J-02 - Files dropped on the window, handled in each program
- **kind**: duplicate. **status**: proposed `keep` (2026-10-21); confirmed (read both). The two handlers do different things on purpose (ld-analyse: one file, and only when nothing is open; ld-stacker-gui: a session and/or many sources, a message for the files left out); what they share is the loop over the URLs and the test of a name. Little to gain; proposed: `keep`, unless the test of a name moves (F-05, A-01).
- **where**: `ld-analyse/mainwindow.cpp:4967-4990` (`droppedCapture`, `dragEnterEvent`, `dropEvent`) and `ld-stacker-gui/mainwindow.cpp:2629-2660`. The check of a
  name is shared (`TbcFiles::isCaptureFileName`).
- **risk**: low.

### J-03 - The frame rate, in ld-analyse
- See A-04: about ten copies of `PAL ? 25.0 : 30000.0 / 1001.0` in one file.

### J-04 - The width adjustment of the aspect ratio, in the two windows
- **kind**: duplicate. **status**: done (2026-10-21); confirmed (`ld-analyse/mainwindow.cpp:1495-1508`, `ld-stacker-gui/mainwindow.cpp:2801-2811`).
- **what**: the same four magic numbers (`103` / `-196` for PAL 16:9 / 4:3, `122` / `-150` for NTSC) are written in both programs; the second says "as ld-analyse does". The
  full screen of ld-analyse does it another way (a real 4:3 / 16:9 ratio from the height, `displayedWidth`).
- **proposal**: one function in `gui-common` (or the library), with the constants named. **risk**: low.
- **done**: `gui-common/aspectratio.h` (`AspectRatio::widthAdjustment(system, widescreen)`) used by both windows; test `testaspectratio` (new), 28/28 unit tests. ld-analyse's full screen keeps its own ratio.

### J-05 - Four analysis dialogs that are one class written four times
- **kind**: duplicate and inconsistent. **status**: done (2026-10-21), the look kept; confirmed (the files were compared with `diff`).
- **where**: `ld-analyse/{dropoutanalysisdialog,visibledropoutanalysisdialog,blacksnranalysisdialog,whitesnranalysisdialog}.{cpp,h,ui}`; `MainWindow::updateGraphs` feeds the four
  one after the other (`mainwindow.cpp:2739-2763`), and their geometry is stored four times in `Configuration`.
- **what**: `dropoutanalysisdialog.cpp` and `visibledropoutanalysisdialog.cpp` differ **only by the names** (70 lines of `diff`, all class name, title, a comment). The black and white SNR
  dialogs differ by a few numbers and, besides, by things that look accidental, not wanted:
  | | black SNR | white SNR |
  |---|---|---|
  | line | 2 pixels, colour from the theme (`isDarkTheme`) | 1 pixel, `Qt::darkGray` |
  | `plot->updateTheme()` after the data | not called | called |
  | clamp (dB) / top of the axis | 20 / 48 | 14 / 42 |
  The class name `VisibleDropOutAnalysisDialog` is also spelt with a capital O where `DropoutAnalysisDialog` is not. The ld-stacker-gui has a fifth and sixth version of the same
  graphs (`SnrGraph`, `DropoutGraph`), with a shared scale.
- **proposal**: one `AnalysisGraphDialog` (title, series name, colour, clamp, axis as constructor arguments) and one geometry key each. Whether the line width and the colour of the two SNR
  graphs should be the same is a question for the person (it changes what is seen). **risk**: low to medium.
- **done**: `DropoutAnalysisDialog(parent, visibleOnly)` serves both dropout graphs (the visible one differs by its title and series name); `SnrAnalysisDialog(Kind)` serves the black and the white SNR (title, series name, clamp 20/14, axis top 48/42, and the two differences of look kept as they were: black 2 px theme colour, white 1 px dark grey and `updateTheme()` at the end). 4 classes and 4 sets of .ui/.cpp/.h became 2, about 700 lines less; the keys of the geometry in the settings are unchanged. Build ok; ld-analyse not run (GUI): to be looked at. **Left**: whether the two SNR graphs should look the same (the person).

### J-06 - Two ways to hold a setting
- **kind**: inconsistent. **status**: done with J-01 (2026-10-21); confirmed (`ld-analyse/mainwindow.cpp:2799-2803` and `ld-stacker-gui/mainwindow.cpp:2715`).
- **what**: ld-analyse keeps its settings in a `Configuration` class (read and written as one block with `writeConfiguration`); ld-stacker-gui writes each one with `QSettings().setValue`
  where it changes. The keys are spelt differently for the same thing (`themeMode`, `styleName`, `customPalette` in the two). Moving the theme menu to `gui-common` (J-01) needs one
  of the two ways, or a small interface.
- **proposal**: decide at J-01. **risk**: low.

### J-07 - Two plotting widgets
- **kind**: redundant. **status**: proposed `keep` (2026-10-21); confirmed (sizes and classes read).
- **where**: `ld-analyse/plotwidget.{h,cpp}` (900 lines; `PlotWidget` on a `QGraphicsScene`: series, grid, marker, legend, axis labels, zoom) and `ld-stacker-gui/snrplot.{h,cpp}` (402 lines;
  `SnrPlot` painted by hand with `QPainter`, `SnrGraph`, `DropoutGraph`). Both draw curves against frame numbers with a marker that follows the frame, a grid, a scale in dB or in samples, and a click
  that asks for a frame.
- **what**: two ways to draw the same kind of graph, two looks, two behaviours of the scale (ld-analyse: zoom, clamp of the SNR to a minimum; ld-stacker-gui: a fixed shared scale, a bridge over
  gaps). They also have two sets of colours for the curves (theme-aware in the second, partly fixed in the first, see J-05).
- **proposal**: to be decided: a plot in `gui-common` that both use (the bigger job of the list), or keep both and document why. **risk**: high (visible look; ld-analyse's live graphs).

## K. Threads and background work

### K-01 - A "pool" class per tool, near copies of each other
- **kind**: duplicate. **status**: partly done (2026-10-21); confirmed for two (ld-process-vits and ld-process-vbi); to verify for the others.
- **where**: `src/ld-process-vits/processingpool.cpp` (142 lines) and `src/ld-process-vbi/decoderpool.cpp` (148 lines): after renaming the class they differ by 36
  lines (`getInputField`, `setOutputField`, `process`). Others of the same shape: `ld-dropout-correct/correctorpool.cpp` (492), `ld-disc-stacker/stackingpool.cpp`
  (677), `ld-chroma-decoder/decoderpool.cpp` (305).
- **found (second look)**: `stackingpool.cpp` and `correctorpool.cpp` hold **five functions that are the same text** after renaming the class: `setMinAndMaxVbiFrames` (85 lines),
  `convertSequentialFrameNumberToVbi`, `convertVbiFrameNumberToSequential`, `writeOutputField`, `historyWith` (113 lines in all); `getAvailableSourcesForFrame` differs (39 and 19 lines).
  `setMinAndMaxVbiFrames` is the VBI scan of A-01.
- **proposal**: a library base for "read the metadata and the video, hand out fields to threads, write the metadata back", and a function `discFrameRange(sources)` for the part about the VBI. **risk**: medium.
- **done**: (a) `FieldPool` (`library/tbc/fieldpool.{h,cpp}`) replaces `ProcessingPool` (ld-process-vits) and `DecoderPool` (ld-process-vbi): the program says the name, the lines read, the worker and the part of the metadata it changes; the same messages. (b) `DiscNumbering::SourceRanges` (`scan`, `contains`, `toDisc`, `toSequential`) replaces the three vectors and the 50 lines of `setMinAndMaxVbiFrames` of `StackingPool` and `CorrectorPool`. Tests: `testfieldpool` (new), `testdiscnumbering` extended; build ok, `ctest -L unit` 30/30; outputs identical to before (vits and vbi on 5 captures: the metadata and the messages and exit codes; stacker and corrector: the `.tbc`, `.tbc.db`, messages and ranges). **Left**: `writeOutputField` and `historyWith` (the same text in the two pools, but they use the pools' own members), `getAvailableSourcesForFrame`, and `ld-chroma-decoder/decoderpool.cpp` (another shape).

### K-02 - Workers in the GUIs
- **kind**: inconsistent. **status**: partly done (2026-10-21); confirmed (read `framebackend.cpp:725-870`).
- **found**: `computeStackSnr` and `computeSourceSnr` hold the same chunk loop (N low-priority threads, an atomic counter, a check of the generation, `yieldToInteraction`, `wait`, `delete`, a signal, a queued call to itself) with a few names changed; `ld-analyse`'s `FileSnr` is a third loop (sequential, a thread of its own). `vitsLines = 24` is written twice in `framebackend.cpp` (`:636`, `:771`) where the library has `VITS_END_FIELD_LINE = 22`.
- **where**: `ld-stacker-gui/framebackend.cpp` (`QThread::create` per chunk for the SNR of the stack and of a source, two near copies of the same loop), `ld-analyse`
  (`filesnr`, `livesnr`, the audio and EFM threads), the pools above.
- **risk**: medium.
- **done**: `FrameBackend::runSnrJobs` runs the jobs of a chunk on threads of the lowest priority (the loop of the SNR of the stack and of a source); `snrLines` (24) is named once for the three places that wrote `vitsLines = 24`. Build ok, `ctest -L unit` 30/30; the SNR measure of ld-stacker-gui not run (GUI): to be looked at. **Left**: `ld-analyse` `FileSnr`/`LiveSnr` (a thread of their own, another shape) and the audio and EFM threads.

## L. The converters and exporters

### L-01 - TBC to CVBS, CVBS to TBC, and what `ld-container` and the library do
- **kind**: redundant, or inconsistent. **status**: proposed `keep` (2026-10-21: the sample conversion is shared since F-03, merging the two programs changes the command line); confirmed (read `ld-tbc-to-cvbs/main.cpp`, `ld-cvbs-to-tbc/main.cpp`).
- **found**: both are a copy loop from a `SourceVideo` to a `TargetVideo` (field by field, with a level map and the `isYc` pair of files handled twice, once for the luma and once for the chroma), over the metadata read and written with `LdDecodeMetaData`; the container work is in the library (`TargetVideo` chooses CVBS or TBC from the output name). Other tools that copy a capture to a new file (`ld-dropout-correct`, `ld-discmap`, `ld-disc-stacker`) have their own loop too, and `ld-container` another one for the LDX. Two thin programs that could be one (`ld-convert`?) is a change of the command line: to be asked.
- **where**: `ld-tbc-to-cvbs` (361 lines), `ld-cvbs-to-tbc` (238), the CVBS output of `TargetVideo`, the conversion of `ld-container pack` and `unpack` (`main.cpp:427,1720`),
  and `ld-discmap`'s own CVBS paths (`discmapper.cpp:730-770`).
- **proposal**: see F-03. **risk**: medium.

## M. Documentation, build and tests

### M-01 - Tools with no page in `docs/Tools/`
- **kind**: inconsistent. **status**: done (2026-10-21, decided with the person: one page for the four converters); confirmed (`docs/Tools` was listed).
- **where**: `ld-json-converter`, `ld-sqlite-to-json`, `ld-tbc-to-cvbs`, `ld-cvbs-to-tbc`, `ld-export-decode-metadata` (a README in its directory only) and `ld-process-ac3`
  have no page there (`ld-chroma-encoder`, `ld-lds-converter` do).
- **proposal**: a page each (or one for the converters), after the points above. **risk**: low.
- **done**: `docs/Tools/converters.md` (the four converters), `docs/Tools/ld-export-decode-metadata.md`, `docs/Tools/ld-process-ac3.md`, written from the `--help` of the built programs.

### M-02 - How a session is read is described in five places
- **kind**: inconsistent. **status**: looked at again (2026-10-21): proposed `keep`; confirmed (`git grep` of the follow-map wording: the specification, `ld-analyse.md`, `ld-chroma-decoder.md`, `ld-disc-stacker.md` and `ld-stacker-gui.md` each say it in their own words; how the sources are lined up is told in the same pages).
- **where**: `docs/Tools/ld-stacker-gui.md`, `docs/Tools/ld-disc-stacker.md`, `docs/Specifications/container-format.md` (section 13), `docs/Tools/ld-analyse.md` and
  `ld-chroma-decoder.md`. When one changes the others must too.
- **proposal**: the specification is the one place; the pages link to it. **risk**: low.
- **second look**: the pages of the tools each describe their own side of a session (what ld-analyse shows, what ld-chroma-decoder decodes, the options of ld-disc-stacker, the window of ld-stacker-gui) and point to each other and to the specification; no contradiction was found. Proposed: `keep`.

### M-03 - The pinned revision of ezpwd, written in six places
- **kind**: duplicate. **status**: deferred (2026-10-21, the person: it can only be checked in CI) (`git grep 62a490c1`).
- **where**: `.github/workflows/package-linux.yml:49`, `package-macos.yml:44`, `package-windows.yml:45` (each with the same "Fetch ezpwd-reed-solomon headers" step of five lines), `build.bat:59`,
  `build.sh:31`, `flake.lock:10`. AGENTS.md section 3 says to keep them in step, by hand.
- **proposal**: one file (e.g. `ezpwd.rev` read by the three scripts and the three workflows), or one reusable workflow step; `flake.lock` stays the authority. **risk**: low (CI only; it can only be
  validated there).

### M-04 - Two pages for each tool, which disagree
- **kind**: duplicate and inconsistent. **status**: partly done (2026-10-21, decided with the person: fix the differences, keep both pages).
- **where**: `src/<tool>/README.md` and `docs/Tools/<tool>.md` for `ld-process-vbi` (202 and 54 lines), `ld-process-vits` (243 / 44), `ld-dropout-correct` (101 / 51), `ld-discmap` (95 / 54),
  `ld-process-efm` (56 / 138), `ld-chroma-decoder` (153 / 318), `ld-disc-stacker`, `ld-analyse`, `ld-export-metadata`, `ld-lds-converter`.
- **what**: each option of the code is found in both (checked with the list of options: all present except `--chroma-weight`, missing in `src/ld-chroma-decoder/README.md`). They are not the same
  text: `docs/Tools/ld-chroma-decoder.md:64` gives `--first-active-field-line` (hyphens) where the program has `--first_active_field_line` (underscores); the page of `ld-process-vbi` has `-n` described in
  one and not in the other.
- **proposal**: one page per tool (`docs/Tools/`), and the `README.md` of the directory a pointer to it. **risk**: low.
- **done**: the long options of every program (from its `--help`) were compared with its two pages. Fixed: `docs/Tools/ld-chroma-decoder.md` (the underscores of the field and frame line options, the decoder option is `-f --decoder` with the real default and lower case names, the luma decoder option that does not exist is gone, the frame line texts and the PAL range), `src/ld-chroma-decoder/README.md` (`--chroma-weight`), `src/ld-dropout-correct/README.md` (no `--source`, `--input-dropouts` or `--output-dropouts`: the sources are the inputs), `src/efm-decoder/tools/vfs-verifier/README.md` (rewritten: it had options and reports the tool does not have), `docs/Tools/ld-discmap.md` (no `--output-metadata`), `src/ld-discmap/README.md` (`--meta`), `src/ld-process-ac3/README.md` (the options of `ld-ac3-demodulate`), the `--no-timecodes` line of three EFM READMEs. **Left**: the texts and the defaults of the options were not compared line by line, and the sub-commands of `ld-container` are not in `--help`.

### M-05 - The list of tools in AGENTS.md
- **kind**: inconsistent. **status**: done (2026-10-21); confirmed.
- **what**: the table of section 1.1 of AGENTS.md (and the layout of 1.2) has no `ld-stacker-gui`, no `ld-container`, no `gui-common`; the `unit` label in section 5 is said to be
  `src/library/**/test*`, while 16 tests carry it: the library's (`testfilter`, `testaudio`, `testcvbs`, `testldx`, `testlinenumber`, `testmetadata`, `testtbcfiles`, `testvbidecoder`,
  `testvitcdecoder`) and also `ld-analyse/test{remote,shortcuts,videofilter,waveform,weave}`, `ld-discmap/testframe`, `ld-stacker-gui/teststackproject` (this last one tests `StackProject`,
  `StackedVideo` and `LdxCapture`, which are library code: the test is not in the library's tree). `TESTING.md:12` lists five tests only.
- **proposal**: update AGENTS.md and TESTING.md; move `teststackproject` to `library/tbc` (its library part) when the points on sessions are done. **risk**: low.
- **done**: AGENTS.md (the table has `ld-stacker-gui`, `gui-common`, `ld-container`; the `unit` label is for `src/**/test*`) and TESTING.md (the 30 tests). `teststackproject` stays where it is.

### M-08 - Unit tests that checked nothing
- **kind**: inconsistent (a test infrastructure fault). **status**: done (2026-10-21).
- **found**: `testvbidecoder` (35 `assert`), `testvitcdecoder` (21), `testlinenumber` (20), `testfilter` (3), `testmetadata`, `testcvbs` and `testldx` check with `assert()`, and the Release and RelWithDebInfo builds (the build of `build.bat`, and the CI one) define `NDEBUG`, which removes `assert()`: those tests ran, passed and checked nothing. (`-DNDEBUG` is in their compile command in `build.ninja`.)
- **done**: `target_compile_options(<test> PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)` for the seven; the binaries now hold the assertion messages and all 17 tests pass; `TESTING.md` says it. The new tests use a macro (`CHECK`) that does not depend on `NDEBUG`.
- **left**: that the same is true for the functional tests was not looked at (they are Python).

### M-07 - Library code that no test includes
- **kind**: inconsistent (the rule of AGENTS.md says a library change has a test). **status**: confirmed for "no test file includes the header"; indirect cover (a test that goes through another class) was not measured.
- **what**: no file under a `test*` directory includes the header of: `dropouts`, `vitsmetrics`, `fieldvbi`, `stackercore`, `sourcevideo`, `targetvideo`, `metadataoptions`, `navigation`, `sqliteio`, `jsonio`,
  `efmchain`, `ldxstream`, `ldxfilter`, `logging`, `filters`, `iirfilter`, `videoid`, `videoiddecoder`, `vitccode`, `biphasecode`, `fmcode`, `closedcaption`, `whiteflag`, `vbiutilities`.
  (`testmetadata` reaches `sqliteio` and `jsonio` through `LdDecodeMetaData`; `testvbidecoder` and `testvitcdecoder` reach some of the codes.) The ones the points of the work list touch (`dropouts`,
  `vitsmetrics`, `stackercore`, `sourcevideo`, `targetvideo`, `metadataoptions`, `navigation`) need **characterisation tests before** they are moved (plan, phase 3).
- **proposal**: the tests are written at the point that needs them, not all at once. **risk**: low.

### M-06 - Helpers copied between the two test drivers
- **kind**: duplicate. **status**: deferred (2026-10-21, the person: Python and CI cannot be run here) (`scripts/test-chroma`, `scripts/test-decode-pretbc`).
- **what**: `resolve_tool`, `safe_unlink` and `clean` are defined in both; the first line differs (`#!/usr/bin/python3` and `#!/usr/bin/env python3`).
- **proposal**: a small module beside them. **risk**: low (the functional tests run in CI only here).

## N. Consistency across the tools

The options of the tools are listed in [audit-refactor-options.md](audit-refactor-options.md) (read from the source). Entries N-01 and N-02 were first seen on the way (`to verify`);
N-03 to N-08 come from that listing and are `confirmed` (the definitions were read):

### N-03 - The same short option, a different meaning
- **kind**: inconsistent. **status**: decided (2026-10-21, the person): no change to the short options (`-r`, EFM names), noted; confirmed.
- **what**:
  | short | meanings |
  |---|---|
  | `-r` | reverse the field order (`ld-chroma-decoder`, `ld-dropout-correct`, `ld-disc-stacker`, `ld-discmap`) **but** the audio sample rate (`ld-tbc-to-cvbs`, `ld-cvbs-to-tbc`, long name `--samplerate`) |
  | `-t` | threads (all the others) **but** "no time stamps" in `ld-process-efm` (`--time`) |
  | `-s` | start frame (`ld-chroma-decoder`), "no strict checking" (`ld-discmap`, `--nostrict`), silence the audio (`ld-process-efm`) |
  | `-p` | pad (`ld-process-efm`), pack (`ld-lds-converter`), output format (`ld-chroma-decoder`), input format (`ld-chroma-encoder`) |
  | `-f` | the decoder (`ld-chroma-decoder`) **but** the video system (`ld-chroma-encoder`, `--system`) |
  | `-n` | do not make a backup of the metadata (`ld-process-vbi`, `ld-process-vits`) **but** no audio (`ld-discmap`, `--no-audio`) |
  | `-o` | the filter-map overlay (`ld-chroma-decoder`, `--oftest`), over-correct (`ld-dropout-correct`), the output file (`ld-lds-converter`) |
  | `-b` | black and white (`ld-chroma-decoder`) **but** data instead of audio (`ld-process-efm`) |
- **proposal**: a short form means one thing in the whole suite (`-r` reverse, `-t` threads, `-i`/`-o` input and output), or no short form at all for the rare ones. Changing a short form is
  a change of the command line: to be asked, one tool at a time, and scripts that use it are kept working by keeping the old short form until the end of a deprecation period. **risk**: medium.

### N-04 - The same job, a different long name (more of N-01)
- **kind**: inconsistent. **status**: partly done (2026-10-21, decided with the person); confirmed.
- **what**: input and output of the metadata converters: `ld-json-converter` has `--input-json` / `--output-sqlite`, `ld-sqlite-to-json` `--input-sqlite` / `--output-json`, `ld-export-decode-metadata`
  `--input-sqlite` / `--output-json` with a different default suffix (`-export.json`, where `ld-sqlite-to-json` writes `.json`), while every other tool has the shared `--input-metadata`. Also
  `ld-process-efm` / `efm-decoder-*` (see I-02), the lines of a frame (`--ffll`, `--first_active_field_line`: with underscores, where all the other long names have hyphens), and the debug options
  (`--debug-efmtof3frames`, `--show-*-debug`, `-V --verbose`, the shared `-d --debug`: four ways to ask for more messages).
- **proposal**: see the table of the options; one name per job, the old kept as an alias. **risk**: low to medium.
- **done**: the four line options of `ld-chroma-decoder` have hyphenated names (`--first-active-field-line`, ...) beside the underscored ones, which work as before (help, page and README show both). Not done: the input and output names of the metadata converters (`--input-json`, `--output-sqlite`, `--input-sqlite`, `--output-json`) and the `-export.json` suffix, the debug options of the EFM tools (kept with I-02).

### N-05 - The same sentence, written differently
- **kind**: inconsistent. **status**: done (2026-10-21, decided with the person: one sentence per shared option); confirmed.
- **what**: the help text of the same option is not the same: threads is "(default number of logical CPUs)" in `ld-chroma-decoder` and "(default is the number of logical CPUs)" in the
  others; `--no-timecodes` says "Process input EFM data with no timecodes (may increase error rates)" in `efm-decoder-f2` and another sentence in the three other `efm-decoder-*`; the
  positional argument for the same file is "Specify input TBC file", "Specify input TBC file (- for piped input)", "Specify input RGB/YCbCr file (- for piped input)",
  "The .tbc file" (`ld-container`, without `Specify`), "Specify input metadata file"...; `-r` is "Reverse the field order to second/first (default first/second)" in four tools, but the
  sample rate in two others with two different sentences.
- **proposal**: a message for each shared option, taken from the library (as `MetadataOptions` already does), the others reviewed together. **risk**: low.
- **done**: the shared options (threads, reverse, no backup, the piped input, the metadata options) already have one sentence each since F-01 and F-04; the help of every program was listed (`--help` of 23 programs) and the rest looked at: `--show-all-debug` of the four `efm-decoder-*` now says "Show all decoding debug" in all four. Left as they are, on purpose: `--no-timecodes` of `efm-decoder-f2` (an option of that stage) against the three others (the chain option for an LDX input), the default of `--output-metadata` (differs between the tools that write in place and those that write a new file), and the positional arguments (each says what its tool reads).

### N-06 - What every tool has: version, help, debug, quiet
- **kind**: inconsistent. **status**: done for ld-container and ld-stacker-gui (2026-10-21, decided with the person); confirmed.
- **what**: `ld-container` has no `--version` and no `-d` / `-q` (its sub-commands only add the help); the two `ld-process-ac3` programs use `getopt`; `ld-disc-stacker` does not use
  `addHelpOption()` (the line is commented out) but defines its own `-? -h --help`; its description says "(c)2020-2025 Simon Inns" while the others have no such line or another one;
  `ld-stacker-gui` has help and `--version` but no `-d` / `-q` (`ld-analyse` has them).
- **proposal**: one function in the library that sets up a tool's parser (help, version, debug, quiet, description header); the description lines reviewed. **risk**: low.
- **done**: `ld-container` (every command, and `-v/--version` before the command) and `ld-stacker-gui` take `-d` and `-q` (and `--version` for ld-container) like the others. `ld-disc-stacker` kept as it is (its own `-h`). **Left**: a function that sets up a parser for all the tools; the "(c)" lines of the descriptions.

### N-07 - Words
- **kind**: inconsistent. **status**: partly done (2026-10-21, decided with the person: glossary, docs and messages; the labels of the windows listed to decide); confirmed for the spelling (counted with `git grep`: "dropout" 589 in the code and 102 in the docs, "Dropout" 463 and 18, "DropOut" 300 in the code (class and function names, not shown), "drop-out" 17 and 5, "drop out" 10; "timecode" 332 against "time code" 38, "time-code" 14; "timestamp" 68, "time stamp" 5, "time-stamp" 10; "field order" 66 against "field-order" 7). The menus of the two windows were compared: the same job has other words: "Toggle auto-resize on zoom" and "Resize frame with window" (ld-analyse) against "Scale window with zoom" and "Resize on first file import" (ld-stacker-gui); "Open file..." against "Open session..." and "Add source..."; "Dropout analysis..." (a window) against "Dropout graph" (a window too); "Process..." (ld-stacker-gui) against nothing in ld-analyse; the capitals are not kept in ld-analyse itself ("Save Metadata", "Black SNR analysis...", "Visible Dropout analysis...", "Closed Captions...", "Video parameters..."); the ellipsis and the accelerator `&` are used in one window and not in the other.
- **what seen**: "capture" / "source" / "track" / "file" for the same thing in the two windows; "drop-out" / "dropout" / "DropOut" (the class `VisibleDropOutAnalysisDialog`); "Frame" vs "Field"
  in the status bars; "Update SNR" vs "Measure"; "Process" vs "Export" for writing the result.
- **proposal**: a short glossary in `docs/`, then one pass over the windows and the docs. **risk**: low (labels only), but each label is asked.
- **done**: `docs/Development/glossary.md` (the spelling, the terms capture, source, track, session, stack, mapped, and the table of the labels of the two windows that differ); "drop-out", "time code", "time-code", "time stamp", "time-stamp" became "dropout", "timecode", "timestamp" in 14 pages of `docs/` and `README.md` files and in 5 messages of the tools (`ld-discmap`, `ld-dropout-correct`, `ld-process-efm` help, the source range message of the stacking tools). **Left**: the labels of the two windows (asked one by one), the debug messages.

### N-08 - Defaults that differ for the same thing
- **kind**: inconsistent. **status**: proposed `keep` (2026-10-21); confirmed (see F-04 for the options of a piped input; the other defaults are in the options table).
- **what seen**: the buffer of a piped input (`--buffer-frames`: frames read ahead, default 32, in `ld-chroma-decoder`; "the size of each of the two buffers, in frames, default 5 seconds of video"
  in `ld-analyse`); the same options for a piped input without metadata are copied word for word between the two (`--input-format`, `--system`, `--black-level`, `--white-level`,
  `--subcarrier-locked`, `--sample-encoding`).
- **proposal**: the options of a piped input become one shared set (like `MetadataOptions`); the two buffer defaults are looked at. **risk**: low to medium.

### N-09 - The exit code of a failure
- **kind**: inconsistent. **status**: done (2026-10-21), decided with the person
- **what**: a failure returns `-1` (which a shell sees as 255) in `ld-analyse`, `ld-chroma-decoder`, `ld-lds-converter`, `efm-stacker-f2`; `1` in `ld-container`, `ld-export-metadata`, `ld-process-efm`, and
  the four `efm-decoder-*`; **both, in the same program**, in `ld-cvbs-to-tbc` (7 and 12), `ld-tbc-to-cvbs` (9 and 11), `ld-disc-stacker` (17 and 4), `ld-discmap`, `ld-dropout-correct`,
  `ld-process-vbi`, `ld-process-vits`, `ld-sqlite-to-json`, `ld-json-converter`, `ld-export-decode-metadata`. In the second group the rule seems to be -1 for a bad command line and 1 for a failed
  job, but it is not kept (e.g. `ld-export-metadata` returns 1 for a missing argument).
- **proposal**: one rule (e.g. 2 for a command line that is wrong, 1 for a job that failed), written in `docs/` and applied by one helper. A change of exit codes can break scripts: to be asked. **risk**: medium.
- **decided (with the person)**: 0 = done, 2 = the command line is wrong, 1 = the work failed. **done**: the 45 `return -1` of the `main()` of 17 tools became 2 (35: a value or an argument that is not valid, options that cannot go together, an output that would be a conversion, two inputs of a different container) or 1 (10: a file that cannot be read, a stream that cannot be opened), and 14 `return 1` that were command line errors became 2 (`ld-process-efm`, `ld-export-metadata`, `vfs-verifier`, the four `efm-decoder-*`: too many arguments, an option that cannot go with another...). A file that cannot be opened, written or that exists already is 1. Written in AGENTS.md (4.4). Checked: `-t 0` and `-t abc` of 5 tools and 7 bad values of the piped input of `ld-chroma-decoder` and `ld-analyse` give 2 (they gave -1, seen as 255 or 127); the results of the stack and of the correction on 5 pairs are unchanged. **Not done**: `ld-container` (68 `return 1`, all its sub-commands) and `ld-process-ac3` (getopt) keep 1; the list of every site is not kept.

### N-10 - Which tools use the shared metadata options
- **kind**: inconsistent. **status**: proposed `keep` (2026-10-21): the tool reads the format from the file, so `--meta` has nothing to choose; confirmed.
- **what**: `ld-export-metadata` takes the metadata as its one positional argument and reads it with `LdDecodeMetaData::read`; it has no `--meta`, no `--input-metadata`. `ld-export-decode-metadata`
  takes `--input-sqlite`. The tools that use `MetadataOptions` accept `--meta db|json` and `--input-metadata`. Its description has two copyright lines in the style of the file ("(c)2020-2023 Adam
  Sampson / (c)2021-2025 Simon Inns"), others "(c)2025 Simon Inns" or none.
- **proposal**: with N-04 and N-06. **risk**: low.

### N-11 - `ld-process-ac3`: its own options, logging and checks
- **kind**: inconsistent. **status**: done (2026-10-21, decided with the person); confirmed (`ld-process-ac3/decode/main.cpp`, `demodulate/main.cpp`, `logger.hpp`).
- **what**: `getopt` with `-v <level>` (here `-v` is the log level; in every other tool it is `--version`), `-s <window>`, `-h` / `-?`; no long options, no `-d`, no `-q`, no `--version`. It has
  its own `Logger` (`logger.hpp`, with `GLOBAL_LOG_LEVEL` and a stream) instead of `tbc/logging.h`; the level is checked with `assert`, which a release build removes
  (`decode/main.cpp:77`). AGENTS.md says getopt is accepted here; the header of the files is another licence block (2022, other authors, derived work).
- **proposal**: long options and the shared debug/quiet options, with `-v` kept as an alias; the check made with a real error. The licence block stays. **risk**: medium (the tools are used in
  pipes: `ld-ac3-demodulate | ld-ac3-decode`).
- **done**: `ld-process-ac3/standardoptions.hpp` (`takeStandardOptions`): `--help`, `--version`, `-d/--debug`, `-q/--quiet` and the long names `--log-level` (for `-v`) and `--sliding-average-window` (for `-s`) are taken from the command line before getopt; `-v` stays the logging level; test `teststandardoptions` (new). **Left**: their own `Logger` and the `assert` that checks the level (kept: a change of behaviour in a release build), and their `return -1`.

### Already in the list above
- N-01 and N-02 below are still `to verify`.

### N-01 - The same job, a different option name
- **kind**: inconsistent. **status**: confirmed (the table [audit-refactor-options.md](audit-refactor-options.md) was made from the definitions; see N-03, N-04, N-10).
- **where**: the input metadata is `--input-metadata` (shared, `MetadataOptions`) in most tools, but `ld-json-converter` has `--input-json` and `--output-sqlite`, `ld-sqlite-to-json` has
  `--input-sqlite`, and `ld-export-decode-metadata` its own input and output names. `-n/--nobackup` exists in two tools; to find out what the others do about a backup of the metadata.
- **proposal**: a table of every option of every tool (name, short, default, check, message) and one rule for each.

### N-02 - Names of the files a tool writes
- **kind**: inconsistent. **status**: moved to the audit (2026-10-21, the person: the names of the files written are for step 2); confirmed (found with `git grep`).
- **found**: the backup of the metadata is `.vbup` for both `ld-process-vbi` and `ld-process-vits` (the same 8 lines in each `main.cpp`, `main.cpp:156` and `:156`; "vbup" is "VBI backup": wrong for VITS), `.bak` in `ld-stacker-gui` (`mapdialog.cpp:438,502`, a different thing: the LDX file put aside), `.mapping-tmp.tbc` (a folder name inside the same dialog), `_map` (a mapped copy, in `ld-stacker-gui` and read by `StackSource::resolvedReference`; `ld-discmap` writes whatever output name it is given), `.export.json` (`ld-export-decode-metadata`, where `ld-sqlite-to-json` writes `.json`), `_audio_0.wav` (the audio of a CVBS capture), `.stacking` (a stream and a temporary stack). The two backups are the same code: a helper in `MetadataOptions` ("the output defaults to the input; back it up unless told not to") is the obvious one.
- **where**: `_map` (a mapping), `.mapping-tmp` (ld-stacker-gui, a folder), `.stacking` (a stream and a temporary stack), `.bak` and `.vbup` (backups), `.export.json`, `_audio_0.wav`.
- **proposal**: list them, and see whether there is a pattern to keep.

## O. Code that nothing uses

Found by listing the functions declared in the headers of the library, `gui-common` and the tools, and counting the places where each name is found in `src/` (the script is not kept; the
list below was then checked name by name with `grep -w`). The slots named `on_<widget>_<signal>` are not in the list: they are connected by name by Qt. Everything here is `confirmed`
(no caller in `src/`), unless a test is named.

### O-01 - Declared, or defined, and never called
- **kind**: dead. **status**: done (2026-10-21); confirmed.
- **ld-analyse**: `AudioConfigDialog::setImportedName`; `AudioEngine::hasFieldSamples`; `Configuration::getMainWindowScaleFactor` / `setMainWindowScaleFactor` (a setting that nothing reads or
  writes); `MainWindow::overlaysVisible`; `PlaybackController::getLoop`; `PlotWidget::removeMarker`, `removeSeries`, `resetZoom`, `setAxisAutoScale`, `setGridPen`, `setLegendEnabled`,
  `updateAxisLabels` (declared, not even defined), the signal `seriesClicked` (never emitted), `PlotMarker::setLabel`; `TbcSource::generateChromaImage`, `generateMonoImage` (declared, not defined),
  `getWeavePlayback`, `setFieldView`; `VideoFilter::getGraph`; `WaveformPanel::controlsVisible`.
- **ld-chroma-decoder**: `Comb::filterIQFull` (declared, not defined), `OutputWriter::getPixelFormat`.
- **ld-stacker-gui**: `MapDialog::succeeded`.
- **library**: `TargetVideo::isLdxStream`; `TbcFiles::isLumaFile`; `openDebugFile` / `closeDebugFile` (`logging.h`: nothing opens a debug file); `LineNumber::fromField1` and
  `SourceAudio::getNumberOfSamples` (used only by their unit test).
- **proposal**: remove them, with their tests when a test is the only user; the `Configuration` entry has a stored value that is then left in the user's settings (harmless). **risk**: low.
- **done**: the 28 names of the list are removed (declaration and definition, and the line of `testlinenumber` and of `testaudio` that used `fromField1` and `getNumberOfSamples`); a second search for each name found no other user. Build ok, `ctest -L unit` 31/31, both windows start. Left in place: the stored `mainWindowScaleFactor` of `Configuration` (nothing reads it now; the value stays in the user's settings).

### O-02 - 43 of the 49 filters of `deemp.h`
- **kind**: dead. **status**: decided (2026-10-21, the person): keep `deemp.h` as it is; confirmed (each `f_` name was looked for outside `deemp.h`; `testfilter` does not use them either).
- **where**: `src/library/filter/deemp.h` (723 lines): "Generated by filtermaker.py on Sat Aug 20 2022 / Do not manually change". Only `f_a40h_48k`, `f_a500_48k`, `f_colorlp`, `f_nr`, `f_nrc` and
  `f_nrpal` are used (by `comb.cpp`, `palcolour.cpp`, `monodecoder.cpp`). The others are the filters of the ld-decode (Python) demodulator that this repository does not have; `filtermaker.py` is not in
  the repository.
- **proposal**: keep only the used ones (the file could no longer be said to be generated, so the first lines are changed too), or leave it as it is as the copy of what ld-decode generates. To be
  asked: the choice is between a smaller file and a file that is the same as upstream. **risk**: low.

### O-03 - Actions of the window of ld-analyse that are in no menu
- **kind**: dead. **status**: done (2026-10-21); confirmed (`mainwindow.ui` has 27 `<action>`; these three have no `<addaction>`; `mainwindow.cpp:1069-1071`).
- **what**: `actionNTSC` ("NTSC..."), `actionVideo_metadata` ("Video metadata...") and `actionVITS_Metrics` ("VITS Metrics...") are defined and only enabled or disabled in `setGuiEnabled`; no menu
  has them and nothing is connected to them.
- **proposal**: remove the three from the `.ui` and the three lines. **risk**: low.
- **done**: the three actions are out of `mainwindow.ui` and `setGuiEnabled` (they had the shortcuts Ctrl+N, Ctrl+M and Ctrl+E but were in no menu, so the shortcuts did nothing). Build ok, `testshortcuts` passes.

## What was read, and what was not

Pass 1 is **finished** (65 entries, sections A to O; all `confirmed` by reading both places or by a count, none left `to verify`). It was made by reading the places where one tool does what another does, by two
searches over `src/` (identical runs of lines between files, [audit-refactor-clones.md](audit-refactor-clones.md); names declared and never used, section O), by the table of the options
([audit-refactor-options.md](audit-refactor-options.md)), and by comparing the pages of `docs/`, the CMake files, the scripts and the CI workflows with the code.

Not read line by line, and said so: the signal-processing inside `ld-chroma-decoder` (`comb`, `palcolour`, `transformpal*`, apart from the noise coring), `ld-chroma-encoder`, the stages of the two EFM stacks (read
only where they share text), `ld-process-ac3`'s filters, the drawing code of `plotwidget.cpp`, the oscilloscope, vectorscope and remote of `ld-analyse`, `ld-compress`. Nothing found there on the way
does duplicate another place (the search found none); step 2 will read them for validity.

What the second pass (phase 4) must check again is in the plan.

## Work list (made at the triage, phase 2)

Not made yet.
