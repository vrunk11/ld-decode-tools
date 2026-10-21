# Glossary (branch `audit-refactor`, point N-07)

How a thing is written in the docs, in the messages of the tools and (when the person has decided) in the labels of the windows. Names in the code (classes, functions,
fields, SQLite columns, JSON keys) are not part of it and are not renamed: `DropOuts`, `ClvTimecode`, `isFirstField`... stay as they are.

## Spelling

| Write | Not | Notes |
|---|---|---|
| dropout, dropouts (at the start of a sentence or a label: Dropout) | drop-out, drop out, DropOut | |
| timecode, timecodes | time code, time-code | "Vertical Interval Time Code (VITC)" and "CLV time code" in a label of a window that has not been changed stay: they are names (see below) |
| timestamp, timestamps | time stamp, time-stamp | |
| field order | field-order | The label "Normal Field-order" of ld-analyse is in the list below |

Done on 2026-10-21 in `docs/` and in the `README.md` of the tools, and in the messages of the tools that a person reads (`ld-discmap`, `ld-dropout-correct`,
`ld-process-efm`, `DiscNumbering`); the debug messages and the texts of the windows were not changed.

## Terms

| Term | Means |
|---|---|
| capture | the video of one pass of a disc or tape: a `.tbc` (or `.cvbs`) with its metadata and audio, or a track of an `.ldx` file |
| source | a capture that is one of the inputs of a stack (a row of the list of ld-stacker-gui; `source` in the options and messages of the stacking tools) |
| track | one stream of an `.ldx` container (video, metadata, audio, EFM, AC3) |
| session | a `.stack` file: the sources, their offsets, the stacking options and the output |
| stack | the result of combining the sources frame by frame; "to stack" is the action |
| mapped | a capture whose frames have been put in the order of the disc by `ld-discmap` (`name_map.tbc`, or a mapped track) |
| frame, field | a frame is two fields, first and second; the numbers of a disc are the numbers of frames |

## Labels of the windows that differ (to be decided one by one)

Not changed: each is asked first (rule of the plan). The same job, in the two windows:

| ld-analyse | ld-stacker-gui |
|---|---|
| "Toggle auto-resize on zoom", "Resize frame with window" | "Scale window with zoom", "Resize on first file import" |
| "Open file..." | "Open session...", "Add source..." |
| "Dropout analysis..." (opens a window) | "Dropout graph" (a window too) |
| "Save Metadata", "Black SNR analysis...", "Visible Dropout analysis...", "Closed Captions...", "Video parameters..." (the capitals are not kept inside ld-analyse itself) | the ellipsis and the `&` accelerators are used in this window and not in ld-analyse |
| "CLV time code:" (VBI dialog, status bar), "VITC time code:" | "no frame numbers or time codes" (source info panel) |
| "Normal Field-order", "Reverse Field-order" | |
| "Update SNR" | "Measure" |
| (nothing) | "Process..." (writing the result); "Export" is used for the session |
