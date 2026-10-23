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
