# Audit and refactor plan (branch `audit-refactor`)

This file is the reference for how the work on this branch is done. It is read again at the start of each session on the branch, and
updated as the work goes: nothing is done that it does not say, and what is done is written in the log at the end.

The inventory itself (what overlaps, what is redundant) is in [audit-refactor-inventory.md](audit-refactor-inventory.md).

## Goal

1. **Step 1, the refactor and the harmonisation.** Go over every feature of every tool and of the library, find what overlaps, what is redundant and what does the same job in
   different ways, **and what is said, named, asked or shown in different ways for the same thing**, list it all, then deal with the points one by one, together. The aim
   is to remove the overlaps and also to make the tools **consistent with each other**: the same option has the same name, short form, default, check and message
   everywhere; the same thing has the same word in the programs, the windows, the messages and the docs; the same job is done by the same code. At the end, analyse the
   final state of the branch again.
2. **Step 2, the audit.** Check that each feature is relevant and that it is valid (does what it says, with the right results). Its own plan is written when
   step 1 is done.

Out of scope for step 1: new features, changes of behaviour that nobody asked for, changes of file formats (see the rules).

Harmonising can change what a person sees (the name of an option, a message, a label). It is allowed, but **each such change is asked about first**, and where it can be done the
old name keeps working (an alias, with the old one still in the docs as deprecated) so that scripts do not break.

## Rules

They are those of [AGENTS.md](../../AGENTS.md), and apply to every change on this branch.

- **No git operation that writes** (add, commit, push, tag, branch, reset, stash...) unless it was asked for in that very message. Work is left in the working tree.
  Reading (status, log, diff, show, blame, grep) is always fine.
- **No attribution** to a tool anywhere (commits, comments, docs).
- **A refactor does not change what a tool does.** The same input gives the same output (same video, same metadata, same exit codes, same options). If a point
  can only be done by changing a behaviour, it is written in the inventory as such and **asked about before**, as are changes to a file format, to the metadata
  schema, to a command line option or to a message that scripts may read. Harmonising (the aim of the branch, with removing overlaps) is the one kind of change to what a person sees that is planned, and it follows the same rule: each
  one is asked about first, and an old option name keeps working as an alias where that can be done.
- **A change in `src/library/` has a unit test** in the matching `test*` directory. Tests that pin down the current behaviour (characterisation tests) are
  written **before** the code is moved, and must pass before and after.
- **Docs and specifications follow the code**: the tool page in `docs/Tools/`, and `docs/Specifications/` for a project format (`.stack`, `.ldx`).
- Golden files in `test-data/` are not changed. New source files have the SPDX header. No reformatting of code that is not otherwise changed.
- Shared logic goes in `src/library/` (or `src/gui-common/` for the programs with a window), never in another tool's directory.

## Method

### Phase 0 - Baseline

Before anything is changed: `build.bat` builds, `ctest -L unit` passes, and what can be run of `ctest -L functional` is run (it needs Python 3 with numpy and
ffmpeg; if it cannot be run here, that is written down, not forgotten). Record in the log: the commit, the number of tests, the lines of code per tool.

### Phase 1 - Inventory (list only, nothing is changed)

For each domain below, read the code (not only search it) and write what is found as an **entry** of the inventory, with proof: files and lines, and what
exactly is the same or is different. An entry has:

| Field | What it says |
|---|---|
| id | `A-01`, `B-02`... (the domain letter and a number; never reused) |
| kind | **duplicate** (same code twice or more), **divergent** (the same job done in different ways, with possibly different results), **redundant** (a tool or a feature that another one covers), **dead** (nothing uses it), **inconsistent** (the same thing named, asked or shown in different ways) |
| where | the files and lines |
| what | the facts, in a few lines, and the proof (what was compared, what was run) |
| effect | what it costs today (bugs, differences that a person can see, maintenance) |
| proposal | what could be done, and what it touches (code, command line, format, docs) |
| risk | low / medium / high, and why |
| status | `to verify` (seen, not proven), `confirmed` (proven), `decided: <what>`, `done`, `dropped: <why>` |

An entry is `confirmed` only when the two places were read and compared, or a test or a run showed it. A suspicion stays `to verify`.

Domains to cover (each is ticked here when its entries are written):

- [ ] A. Reading the VBI, frame numbers and timecodes (CAV and CLV), frame rates
- [ ] B. Stacking, mapping and dropout correction across several sources (ld-disc-stacker, ld-discmap, ld-dropout-correct, StackedVideo, ld-stacker-gui)
- [ ] C. Dropouts (the data, correction, drawing, graphs)
- [ ] D. SNR and VITS (measures, per frame aggregation, graphs)
- [ ] E. Metadata (JSON, SQLite, CVBS, LDX; reading, writing, converting, exporting; the two exporters, the converters)
- [ ] F. Opening and writing video and audio (TBC, CVBS, LDX, streams, PCM, WAV); the command line of each tool (options, defaults, messages)
- [ ] G. The LDX container (ld-container, the library, what the GUIs do with it)
- [ ] H. Chroma decoding (ld-chroma-decoder, ld-analyse, ld-stacker-gui) and the picture of a frame (weave, field views, zoom)
- [ ] I. EFM and AC3 (ld-process-efm, efm-decoder, ld-process-ac3, what ld-analyse does with them)
- [ ] J. The programs with a window (ld-analyse, ld-stacker-gui, gui-common): theme, settings, drop of files, plots, windows, file names
- [ ] K. Threads, pools and work done in the background (each tool has its own)
- [ ] L. The tools that convert or export (ld-json-converter, ld-sqlite-to-json, ld-tbc-to-cvbs, ld-cvbs-to-tbc, ld-lds-converter, ld-export-*)
- [ ] M. Build, tests and scripts (CMake, `scripts/`, CI) and the documentation itself (pages that say the same thing twice, or that disagree)
- [ ] N. Consistency across the tools, as a domain of its own: option names, short forms, defaults and checks; messages and exit codes; names of files written (suffixes,
  `_map`, `.stacking`); words used for the same thing (frame, field, capture, source, track, session, stack, mapped); order and names of menus and labels in the two windows;
  how a page of docs is built (usage, options, examples); units and formats of numbers (dB, samples, timecodes)

### Phase 2 - Triage (together)

Go through the inventory one entry at a time with the person. For each: keep as it is, document it, merge, remove, or drop. Record the decision in the entry. Then
fix the order: first what is low risk and used by many things (shared helpers), last what changes how a tool is used. The result is the **work list**, at the
end of the inventory.

### Phase 3 - Doing the points, one at a time

For each point of the work list, in order, and only one at a time:

1. Read the plan and the entry again. Say what will be changed and what will not.
2. Write the tests that pin down the current behaviour (library: unit tests; tools: a run on `test-data` with the output compared; windows: a written list of what
   to try by hand).
3. Make the change. Build (`build.bat`), run `ctest -L unit`.
4. Compare: the outputs before and after are the same (or the difference is the one that was decided).
5. Update docs and specifications. Look at the whole diff.
6. Write the result in the log, and set the entry `done`. **No commit unless it is asked for.**

If a point turns out to be bigger or different from what the entry said, stop, write it in the entry, and ask.

### Phase 4 - Re-analysis of the final state

Do phase 1 again on the branch as it is, with the same domains and the same way of reading, and compare with the first inventory: what was removed, what is left,
what the changes brought (new duplicates, new differences). Redo the baseline numbers (lines of code, tests). Write the result in the log. Anything left over
is listed, with the reason.

### Phase 5 - Step 2, the audit of relevance and validity

Planned when step 1 is done: for each feature, is it still needed, does it do what its page says, is the result right (compared with a reference, a standard, a
test). Own inventory and own plan.

## Checks at every gate

- `build.bat` ends with "Build complete" and no error (a warning that was not there before is looked at).
- `ctest -L unit` passes; the number of tests did not go down.
- No file of a format was changed without the specification being changed (and asked about).
- Docs say what the code does. Nothing in the docs mentions a thing that does not exist.
- The working tree holds only what the point needed.

## Where things stand, and how to resume

Written so that a new session (the conversation may have been compacted) can carry on from this file and the inventory alone.

**State of the repository (2026-10-21)**
- Branch `audit-refactor` is at `4ba94c96`, the same commit as `main` (fast-forwarded on 2026-10-21 at the person's request) and as `origin/main` (pushed): the last commit is the
  measure modes (rough, full, seek) and the format list of the Process window of ld-stacker-gui. The branch itself is not pushed.
- The audit files (`docs/Development/audit-refactor-*.md`) are **untracked** and the refactor changes are **only in the working tree** (nothing is staged or committed, nothing pushed); `test-data/` is untouched.
- Tags: `v1.6.2` is on `0057624f` (published, behind `main`); `v1.6.3` is being prepared and has no tag and no release notes yet (the format of the release notes is the
  one of v1.6.0: `# vX.Y.Z - title`, a paragraph, `## Section` with `- **Label**: text`, a docs link per section).

**What was agreed with the person**
- Step 1 is the refactor **and the harmonisation** (the same name, default, check and message for the same thing in every tool, window and page; the same job done by the same
  code). Step 2, later, is the audit of relevance and validity.
- First make the exhaustive list (the inventory), then deal with the points **one by one, together** (the person decides each), then analyse the final state again.
- Anything that changes what a person sees (an option name, a message, a label, a file name, a format) is asked about first; old option names stay as aliases where possible.
- The person speaks French; the answers are in French; the repository (code, docs, this file) is in English.

**What is done in the inventory (pass 1, not finished)**
- Pass 1 is **finished**: 65 entries in sections A to O (O is dead code), all `confirmed`. Annexes: `audit-refactor-options.md`, `audit-refactor-clones.md`. The triage is made entry by entry together with the doing of each point (phase 3 follows the order of the inventory, A to O): done from A to O (each entry says what was done and what is left); next the re-analysis (phase 4) and the plan of step 2.

**How the work is done here** (things that went wrong before)
- Build from the repository root with `.\build.bat` in PowerShell (not from another folder, and not `cmake` from Git Bash). It takes minutes: run it in the background and read
  its output file when it is done. Then `ctest -L unit` in `build\`. A `vcpkg` message about a file in use means a program of the build is still running.
- Many source files have CRLF line endings. Edit them with the editing tools, or with a script that reads the file, replaces `\r\n` by `\n`, edits, and puts `\r\n` back;
  `perl -0pi` on one line does not match across the CRLF. Long here-documents with quotes can fail in the shell: write scripts and files with the file tool.
- There is no `python`, no `gh` here. Releases are written as text for the person to paste.
- Do not `rm` with a wildcard.

## Phase 4 - Re-analysis of the final state (2026-10-21)

The same readings as in pass 1, on the branch as committed (`a5089ea2`) compared with `main` (`4ba94c96`, unpacked with `git archive`).

**Numbers**

| | `main` | branch |
|---|---|---|
| unit tests | 16 (the `assert` ones checked nothing, M-08) | 31 (15 new) |
| C++ and `.ui` lines under `src/` (without `deemp.h`) | 99 649 | 99 909 |
| of which `library` / `gui-common` | 21 482 / 831 | 24 161 / 1 442 |
| `ld-analyse` / `ld-stacker-gui` | 23 845 / 7 532 | 22 787 / 7 308 |
| `ld-json-converter` / `ld-export-decode-metadata` | 840 / 2 768 | 329 / 2 069 |
| `ld-process-vbi` / `ld-process-vits` | 538 / 523 | 315 / 302 |
| pairs of files that share 8 or more windows of 5 lines | 3 666 | 3 625 |
| pairs that share 20 windows or more | 24 | 19 |
| inventory entries: done or partly done / proposed keep, decided, deferred or moved to the audit | - | 46 / 19 |

The total did not shrink: the code that was copied has become shared code (and 15 new unit tests are added), so the tools lose lines and the libraries gain them.

**What was removed (the pairs of files that no longer share text)**: `ld-export-decode-metadata/jsonio.*` with the library's (258 + 73 windows), the copies between `ld-analyse` and `ld-stacker-gui` (`mainwindow.cpp`, 27),
the pools of `ld-process-vbi` and `ld-process-vits` (13) and most of those of `ld-disc-stacker` and `ld-dropout-correct` (69 to 37), the two `main.cpp` of the stacker and the corrector (66 to 49).
No pair that involves a file created on this branch has 8 windows or more: the new shared code did not make new copies.

**What is left, with the reason** (the largest pairs now)
- `ld-export-decode-metadata/exportmetadata.cpp` and `.h` against `lddecodemetadata.*` (237 + 55): a second metadata model, kept (E-04).
- the two EFM stacks and the tools of `efm-decoder` between them (148 + 33 + 56 + the `main.cpp` and `decoders.h` of the four tools): deferred by the person (I-01, I-03).
- `ld-disc-stacker/main.cpp` and `ld-dropout-correct/main.cpp` (49), `ld-process-vbi/main.cpp` and `ld-process-vits/main.cpp` (39): the same steps of a tool that processes a capture (F-06, "left": a helper for them was not made).
- `stackingpool.cpp` and `correctorpool.cpp` (37): `writeOutputField`, `historyWith` and the metadata of the output use the pools' own members (K-01, "left").
- `ld-ac3-demodulate` and `ld-ac3-decode` (37), `transformpal2d` and `3d` (26), `ntscencoder` and `palencoder` (23): not looked at in step 1 (the filters and the encoder).

**Names declared and used nowhere**: the same search over the headers gives 145 names with two uses or fewer instead of 170; 117 of them are the filters of `deemp.h` (kept) and slots that Qt connects by name. The other 28 are to be looked at in step 2
(`ld-analyse`: `trimBefore`, `clearSkipped`, `reachedEnd`, `hasFailed`, `setLabel`, `setSilenced`, `setSolo`; `ld-chroma-decoder`: the accessors of `ComponentFrame`, `getOutputWriter`, `setComponentFrame`;
`ld-discmap`: `getAudioFieldSampleLength`; `ld-stacker-gui`: `mappedIds`, `setSmooth`; library: `makeLive`, `writeData`, `fromField0`; and the overrides `wheelEvent` and `leaveEvent`, which Qt calls).
One name added on this branch and never used (`PipedInputOptions::isSubcarrierLockedSet`) was removed.

**Differences that the branch created**: the exit codes (decided), the 8 field phases of PAL in the stack metadata, the `.tbc.db` schema text written by the library, the blanking value of `ld-json-converter` for an NTSC JSON without
`blanking16bIre` (E-03: to be confirmed), hyphenated names beside the underscored ones for four options, `-d`/`-q`/`--version` added to three programs, the 3 menu shortcuts that did nothing. Each is in its entry.

**Not checked on this machine** (and so to be looked at by the person or in CI): the windows of `ld-analyse` and `ld-stacker-gui` (menus, theme editor, analysis graphs, SNR measure, dropout drawing), the functional tests (`ctest -L functional` needs Python and ffmpeg),
the Windows/MSVC and macOS builds (`getopt` of `ld-ac3-*`, the new files in the CMake lists).


## Log

| Date | Phase | What was done | Result |
|---|---|---|---|
| 2026-10-21 | - | Branch `audit-refactor` created from `main` at `7bf9a4cf`; plan written | - |
| 2026-10-21 | 1 | Inventory pass 1 begun on every domain (A to M): 34 entries (J-03 only points to A-04), of which 14 `confirmed` and the rest `to verify`; not finished (see the end of the inventory) | The ld-stacker-gui changes that were in the tree when the branch was made were committed on `main` (`4ba94c96`, pushed); the branch was brought up to date with `main` by a fast-forward (`4ba94c96`) |
| 2026-10-21 | - | `audit-refactor` fast-forwarded to `main` (`4ba94c96`) at the person's request | Branch = main = origin/main; only the two audit files are untracked |
| 2026-10-21 | 1 | Chat compacted; AGENTS.md read again. Pass 1 continued: ld-analyse (places that copy others), options of every tool (new annex `audit-refactor-options.md`), ld-chroma-decoder, exporters, CI repeats, dead code | Inventory at 55 entries (sections A to O); new: J-04 to J-06, F-04, F-05, H-03, I-02, M-03, N-03 to N-10, O-01, O-02; A-05, C-02, D-01, E-04 and I-01 completed. Nothing in the code was changed |
| 2026-10-21 | 1 | Pass 1 finished at the person's request ("finis l'analyse complète"): the rest of ld-analyse, the chroma decoder, the library, the scripts, CMake, CI and docs read or searched; two searches over `src/` (clones, unused names); every `to verify` entry verified | 65 entries, all `confirmed`; new: B-05, F-06, I-03, J-07, M-04 to M-07, N-11, O-03; annexes `audit-refactor-options.md` and `audit-refactor-clones.md`. Nothing in the code was changed |
| 2026-10-21 | 3 | A-01 done: `DiscNumbering` (library) used by `StackingPool` and `CorrectorPool`; test `testdiscnumbering` added | build ok, `ctest -L unit` 17/17, outputs of both tools on 5 pairs identical to before (md5 of .tbc and .tbc.db, exit codes, messages) |
| 2026-10-21 | 3 | A-02 done (decided with the person): one rule for a disc number, 0 and negative numbers allowed, `NoNumber` for none; used by the library, ld-discmap, ld-stacker-gui; spec section 13 updated | build ok, `ctest -L unit` 17/17; outputs of ld-discmap, ld-disc-stacker, ld-dropout-correct and of sessions (ld-chroma-decoder, ld-disc-stacker) identical to before |
| 2026-10-21 | 3 | A-03 (dead copy deleted), A-04 (rate and time-code functions in the library, used in the library, ld-analyse, ld-stacker-gui) done; M-08 (new, done): the unit tests that use `assert` are built with `-UNDEBUG` | build ok; `ctest -L unit` 17/17 with the asserts really on; outputs of ld-discmap, ld-disc-stacker, ld-dropout-correct and sessions identical to before |
| 2026-10-21 | 3 | A-05 done (format decided with the person: that of ld-stacker-gui, except the remote control): `DiscNumbering::cavText` and `clvText` used by ld-analyse (status bar, VBI dialog), ld-export-metadata (CSV) and ld-stacker-gui; docs of ld-analyse and ld-export-metadata updated | build ok, `ctest -L unit` 17/17; CSV of 5 captures: only the leading zero of the hour differs |
| 2026-10-21 | 3 | B-01 (StackFrame: sessions, preview of ld-stacker-gui, mode and frame of the CLI) and B-02 (field phases, 8 for PAL, decided with the person) done; teststackframe added | build ok, `ctest -L unit` 18/18; sessions and CLI: `.tbc` identical, NTSC `.tbc.db` identical, only the PAL `.tbc.db` differ (phases); decoded images of the PAL session identical; the preview of ld-stacker-gui not run |
| 2026-10-21 | 3 | B-03 checked (nothing to merge beyond A-01, A-02, K-01), B-04 done (and its entry corrected: the GUI gives the .stack to ld-disc-stacker) | build ok, `ctest -L unit` 18/18; sessions identical |
| 2026-10-21 | 3 | B-05 done for stackercore.cpp (teststackercore first: 5 hashes; then the repeated loops extracted) | build ok, `ctest -L unit` 19/19; the 5 hashes unchanged; CLI outputs identical |
| 2026-10-21 | 3 | C-01 (DropOuts::totalLength, visibleLength, coversWholeField), C-02 (DropoutDrawing in gui-common) and C-03 (ld-analyse ignores a frame that ld-discmap could not number, decided with the person) done; testdropouts and testdropoutdrawing added | build ok, `ctest -L unit` 21/21; discmap output, CLI and sessions identical; the picture of ld-analyse compared with the old painter code in a test, the window not seen |
| 2026-10-21 | 3 | D-01 done: `SnrAverage` (library) replaces the 5 averages of ld-analyse and ld-stacker-gui; testvitsmetrics added | build ok, `ctest -L unit` 22/22; the GUIs and the SNR graphs not run |
| 2026-10-21 | 3 | E-01, E-02 (decided with the person: the library takes ld-decode's schema), E-03, E-05 done; E-04 partly (jsonio copy gone, one-definition-rule warning gone, the second model kept for now). testsqliteschema added | build ok, `ctest -L unit` 23/23; the data written by ld-json-converter, ld-disc-stacker and ld-dropout-correct are identical once read back (the `.db` bytes differ: new schema text); one value of the converter to decide (blanking) |
| 2026-10-21 | 3 | F-01 done for -t, -r, -n (`commonoptions` in the library; testcommonoptions added); started group F | build ok, `ctest -L unit` 24/24; exit codes, backups, metadata and help identical but one line of the help of ld-chroma-decoder |
| 2026-10-21 | 3 | F-04 done: `PipedInputOptions` (library) used by ld-analyse and ld-chroma-decoder; testpipedinputoptions added | build ok, `ctest -L unit` 25/25; exit codes, messages and RGB output identical on 9 combinations; only the help of the 6 options is the same text in the two programs now |
| 2026-10-21 | 3 | F-05 partly done (base name and track by the library in SidecarAudio::find; the suffix list stays) | build ok, `ctest -L unit` 25/25 (testwaveform finds the sidecars) |
| 2026-10-21 | 3 | F-03 done for the 10-bit word (`SampleConverter::toWord`, `fromWord`) | build ok, `ctest -L unit` 25/25; CVBS, unpacked and converted files identical to before |
| 2026-10-21 | 3 | N-09 done (decided with the person): exit codes 0 / 2 (command line) / 1 (work), 45 `return -1` and 14 `return 1` changed in the mains of 17 tools; AGENTS.md 4.4. Decided with the person, not done: ld-process-efm keeps its option names (I-02, N-03), `-r` stays the sample rate of the two converters | build ok, `ctest -L unit` 25/25; bad values give 2; processing results unchanged |
| 2026-10-21 | 3 | F-02 looked at again: not merged (proposed keep); F-06 partly done (`SourceFileChecks` in ld-disc-stacker and ld-dropout-correct) | build ok, `ctest -L unit` 26/26; messages and exit codes of 10 cases identical but "output exists" now 1 |
| 2026-10-21 | 3 | G-01 (proposed defer) and G-02 (proposed drop) looked at, recorded in the inventory; no code | - |
| 2026-10-21 | 3 | H-01 partly done (`colourconfiguration.h`: the choice PalColour or Comb and its configuration, 4 places in ld-analyse and ld-stacker-gui); H-04 done (`lumacoring.h`: `coreLumaLine` for Comb, PalColour, MonoDecoder; `addChromaToLuma` in ld-analyse); testlumacoring added | build ok, `ctest -L unit` 27/27; the new function equals the old algorithm bit for bit in the test; `ctest -L functional` cannot be run here (BAD_COMMAND: no Python), the decoder not compared on `test-data` |
| 2026-10-21 | 3 | H-03 done (decided with the person): help and docs say IRE for the noise reduction, the limits and defaults kept, the PAL filter is called `PALcolour (2D)` in both windows; H-02 looked at again: proposed keep | text only; build ok |
| 2026-10-21 | 3 | I-01 (proposed keep, document), I-02 (decided: no change), I-03 (deferred by the person, noted for step 2): no code | - |
| 2026-10-21 | 3 | J-04 done (`gui-common/aspectratio.h`, testaspectratio); J-05 done (4 analysis dialogs of ld-analyse became `DropoutAnalysisDialog(parent, visibleOnly)` and `SnrAnalysisDialog(Kind)`, look kept); J-01 and J-06 done (`gui-common/appearancemenus`, testappearancemenus); J-02 and J-07 proposed keep | build ok, `ctest -L unit` 29/29; both GUIs start offscreen; the windows (menus, theme editor, analysis graphs) not seen: to be tried by the person |

| 2026-10-21 | 3 | K-01 partly done (`FieldPool` for ld-process-vbi and ld-process-vits; `DiscNumbering::SourceRanges` for the stacking and correcting pools); K-02 partly done (`runSnrJobs`, `snrLines` in ld-stacker-gui); testfieldpool added, testdiscnumbering extended | build ok, `ctest -L unit` 30/30; vbi and vits on 5 captures and stacker and corrector on 5 pairs identical to before (metadata, messages, exit codes, md5); the SNR measure of ld-stacker-gui not run |
| 2026-10-21 | 3 | L-01 proposed keep; M-05 done (AGENTS.md, TESTING.md), M-01 done (three pages), M-04 partly (options of every program compared with its two pages, wrong ones fixed), M-02 proposed keep, M-03 and M-06 deferred | docs only; the options of 23 programs checked against `--help` |
| 2026-10-21 | 3 | N-04 (hyphenated names of the four line options of ld-chroma-decoder), N-05 (`--show-all-debug` text), N-06 (`-d`, `-q`, `--version` in ld-container and ld-stacker-gui), N-11 (`ld-process-ac3`: `--version`, `-d`, `-q`, long names; `teststandardoptions`); N-03 decided no change; N-08 and N-10 proposed keep; docs of the EFM tools, ld-container, ld-stacker-gui, ld-process-ac3 and ld-chroma-decoder updated | build ok, `ctest -L unit` 31/31; the old and the new names of the line options give the same picture (md5), the new options of the programs tried by hand |
| 2026-10-21 | 3 | N-07 partly done (glossary `docs/Development/glossary.md`; dropout, timecode, timestamp in the docs and 5 messages; the labels of the windows listed, not changed); N-02 moved to step 2 by the person | build ok, `ctest -L unit` 31/31; stacker and corrector outputs identical (one info message differs: "timecodes") |
| 2026-10-21 | 3 | O-01 done (28 unused functions and a signal removed), O-03 done (3 actions of ld-analyse in no menu removed) | build ok, `ctest -L unit` 31/31; both windows start offscreen |
| 2026-10-21 | 3 | O-02 decided by the person: `deemp.h` kept as it is. **Phase 3 has been gone through in the order of the inventory (A to O)**; what is left of each point is in its entry | - |
| 2026-10-21 | 4 | Re-analysis of the final state: lines per tool, pairs of files that share text (same script on `main` and on the branch), names used nowhere, status of the entries; `PipedInputOptions::isSubcarrierLockedSet` (added and never used) removed | build ok, `ctest -L unit` 31/31; see the section "Phase 4" |
| 2026-10-22 | - | After the commit, at the person's request (a lag with some captures): the metadata and the opening of sources of ld-stacker-gui were measured on synthetic captures (20 000 frames with 100 dropouts a field) and made faster. SQLite read: by column number, forward-only queries, the dropouts built field by field (1 659 ms to 477 ms for 2 million dropouts; the JSON read takes 250 ms); sources are read in the background in several threads (`FrameBackend::loadSource`, `finishLoad`), so that the pictures are not kept waiting and three sources load together; the phase IDs of a CVBS capture read only the lines and samples they use, in up to 4 threads (0.092 to 0.025 ms a frame with the file cached). Tests: `testsqliteread` (new, the old read first), the phase IDs in `testcvbs` (checksums of the old measure) | build ok, `ctest -L unit` 32/32; the ld-stacker-gui window loaded 3 sources offscreen in 1.2 s and never stopped for more than 50 ms; not tried with a real capture |

## Notes for step 2 (the audit of validity and relevance)

What was found on the way and is **not** being fixed by the refactor (the person's rule: bugs found are kept for the audit unless a refactor needs them). Each line says where it is written in the inventory. Add to this list as the work goes; step 2 starts from it.

**Possible wrong results**
- B-02 (fixed with the refactor, decided with the person): `ld-disc-stacker` repaired the field phases of PAL with 4 phases; PAL has 8.
- A-04: `ld-export-metadata/closedcaptions.cpp:49` uses 29.97 for the time of a caption (the exact rate is 30000/1001): about 0.4 s per 100 hours of drift in the SCC files.
- H-03 (fixed as text, decided with the person): the noise reduction level of `ld-chroma-decoder` is in IRE in the code and the two windows; the help and the page said dB, they say IRE now.
- E-05: `LdDecodeMetaData::getField*` / `updateField*` only log an out-of-range field number and then index the vector (undefined behaviour).
- F-03: a CVBS sample goes through the 10-bit word of an LDX track (`toTbc(x) >> 6`): for `s16` (15 bits) and `tpg21` that loses precision; check the specification says a CVBS track is 10-bit.
- A-02: `ld-discmap` corrects the NTSC CLV "offset frames" (`isNtscAmendment2ClvFrameNumber`); the stacking tools and the sessions do not. A capture of an NTSC CLV disc with the jump may be numbered differently by them.
- B-05: in the smart neighbour mode of `StackerCore` (`stackercore.cpp`) the centre is unsigned, so `centre - threshold` wraps when the centre is under the threshold (3840 in 16-bit units, far under the black level of any capture): the samples are then all out of range. In the smart mean mode the centre is signed. Kept as it was by the refactor.
- E-02: the dropouts of a field that are read from a SQLite file come back in the reverse order of the query (`QMultiMap::values` in `readFields`): sorted by `startx` in SQL, then reversed. The JSON reader keeps the file order. Nobody checked that something depends on the order.
- E-03: an NTSC JSON with no `blanking16bIre` is converted by `ld-json-converter` with the blanking level the library works out (it was the black level). Decide if the old value must come back.
- E-05: `LdDecodeMetaData::getField*` / `updateField*` log an out-of-range field number and then index the vector (undefined behaviour); `fieldAt` keeps that.
- F-02: `SourceAudio::open` stops the program (`qFatal`) when the `.pcm` beside a capture is missing or empty; `ld-discmap` has no other way to say it.
- H-03: the noise reduction level is limited to 0..10 in the ld-analyse and ld-stacker-gui windows, but only refused when negative by `ld-chroma-decoder`; kept as is (decided 2026-10-21), to see whether 10 is a real limit.
- H-03: the library default PAL filter is palColourFilter but ld-analyse and ld-stacker-gui both start on transform2DFilter.
- I-03: the readers and writers of the EFM tools differ in end-of-file handling (`reader_data24section` audio vs data; `reader_f2section`, `writer_f2section` d24/f2 vs stacker); the CD sector tables are in three places.
- J-05: the black and white SNR graphs of ld-analyse differ in look (line width and colour, `updateTheme()` only for white), kept as they were; the white pen is set at each `finishUpdate`.
- M-04: the `ld-ac3-decode` help says its output is a stream of QPSK symbols (that is the output of `ld-ac3-demodulate`; the decoder writes the AC3 stream); `ld-export-decode-metadata` describes itself as a "JSON converter tool".
- M-04: the texts and defaults of the options of the two pages of each tool were not compared line by line (only the names).
- N-02: the backup of the metadata is `.bup` in `ld-process-vbi` and `.vbup` in `ld-process-vits`; `ld-export-decode-metadata` writes `-export.json` where `ld-sqlite-to-json` writes `.json`; other suffixes (`_map`, `.mapping-tmp`, `.stacking`, `.bak`, `_audio_0.wav`): to be listed and kept or harmonised in step 2.
- N-04: the input and output option names of the metadata converters (`--input-json`, `--output-sqlite`, `--input-sqlite`, `--output-json`) differ from the shared `--input-metadata` / `--output-metadata`.
- O-02: `src/library/filter/deemp.h` holds 49 filters of which 6 are used; kept as the copy of what ld-decode generates.
- B-03: the in-memory mapping of `ld-disc-stacker` (`--no-map` aside) and `ld-discmap` were compared on `test-data` only (no gaps): equal there; with gaps, repeated frames or unnumberable captures not run.
- A-02: the rule for a disc number is now one (0 and negative allowed), but a capture whose first 600 frames are mostly corrupt was not tested.
- N-11: `ld-process-ac3` checks its log level with `assert` (removed in a release build).
- C-03 / J-05: the two SNR graphs of `ld-analyse` clamp at 20 dB and 14 dB and draw with different pens; whether it is wanted is not known.

**Relevance (does it still serve)**
- O-01, O-02, O-03: dead code, 43 of the 49 filters of `deemp.h`, three actions of the `ld-analyse` window in no menu.
- E-04, L-01, I-01: `ld-export-decode-metadata` (a second metadata model), `ld-tbc-to-cvbs` / `ld-cvbs-to-tbc` (two copy loops), the second EFM stack.
- N-02: the backup file of `ld-process-vits` is called `.vbup` (a VBI name).

**Tests and checks**
- M-08: unit tests that use `assert` were not checking anything (fixed for the 7 that exist); the functional tests (Python) were not looked at for the same fault.
- M-07: library code that no test includes (`stackercore`, `dropouts`, `vitsmetrics`, `sourcevideo`, `targetvideo`, `metadataoptions`, `navigation`...).
- The behaviour of the GUIs is checked by a build and by hand only; none of the changes of the refactor in `ld-analyse` / `ld-stacker-gui` (status bar text, the rate functions, `NoNumber`) was seen on the screen.
