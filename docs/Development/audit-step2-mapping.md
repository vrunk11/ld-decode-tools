# Mapping, in detail (S-01, S-07, S-12 of the findings)

Written 2026-10-22 for the decision on how a mapping should be kept. It is an analysis: nothing here is decided, and nothing was changed in the code.

The principle that the person gave: **keep a clean legacy support, but make things simpler to manage.**

## 1. What a mapping is, from the code

`ld-discmap` builds a `DiscMap`: a list of frames in the order of the disc (`ld-discmap/discmap.h`, `frame.h`). For each frame of the result:

| member | meaning |
|---|---|
| `vbiFrameNumber` | the number on the disc (CAV picture, or CLV time in frames) |
| `firstField`, `secondField` | the fields of the **capture** (numbers from 1) that make the frame; their order in the file says the field order |
| `isPadded` | no frame of the capture: two black fields are written |
| `isUnmappable` | a frame with no number that could not be given one: kept, numbered, and its fields marked as dropouts over the whole width |
| `isPullDown`, `isPictureStop`, `isLeadInOut`, `isClvOffset` | flags kept for the decisions of the mapping; not needed to read the result |
| `frameQuality`, phases | used to choose between duplicates; not needed to read the result |

`saveDiscMap` (`discmapper.cpp:713`) then writes, for each frame, **the two fields of the capture again** (or two black fields), **the audio of those fields** (`.pcm`, or the WAV / track of a CVBS capture; silence for a padded frame), and
`buildTargetMetadata` (`discmap.cpp:799`) writes **the metadata of those fields again with three changes**: `isMapped = true`; the VBI of the first field **rewritten** with the number of the map; the dropouts of an unmappable frame replaced by
a dropout over every line; a padded frame has `pad = true`.

So the information that the mapping adds to the capture is, per frame, one of: *the capture's frame k*, *nothing (padded)*, *the capture's frame k, unusable*, and the number on the disc. The rest of the output is the capture again.

What it costs: the folder of the person (`D:\decodde\vhs_build`) has `decode.tbc` 1 721 740 020 bytes and `decode_map.tbc` 1 721 261 360 bytes (with its `.pcm` and its metadata, the same size again); `test.tbc` 2.59 GB and `test_map.tbc` 2.51 GB.
A map of the same capture needs one entry for each of about 95 000 frames: under a megabyte, as text.

## 2. Who makes it, who reads it

Makes it: `ld-discmap` (command line); the Map window of `ld-stacker-gui` (runs `ld-discmap`, then puts the files in place: `_map`, or in place of the capture with `.mapping-tmp` and `.bak`, or as a track of an LDX file with `mappedFrom`).

Reads it, by one of these three signs:
- the **file name** `<name>_map.tbc` (`TbcFiles::mappedFileName`, used by `StackSource::resolvedReference`, so by `.stack` sessions, `ld-disc-stacker` given a session, `ld-analyse`, `ld-chroma-decoder` given a session, and the Map window);
- the **LDX track** with `mappedFrom` (`LdxCapture::mappedOfOriginal`, `isMappedTrack`);
- the **metadata flag** `isMapped` (`ld-disc-stacker` and `ld-dropout-correct` refuse a source that is not mapped unless `--no-map`; the window shows it and offers Map only when it is false).

What the stacking tools do with a mapped capture: they take frame `n` of each source as the frame with disc number `origin + n - 1`, so that the sources are lined up by index. **That is why they need a mapping**: without it, a repeated or a missing frame
shifts every frame after it. (Correction of the findings: `ld-disc-stacker --no-map` does **not** map in memory. It only drops the requirement, and lines the frames up by the first number, as if there were no gap: `StackingPool::convertVbiFrameNumberToSequential`.
The mapping in memory that I wrote about in S-01 and S-07 does not exist; what exists is the in-memory **metadata** of a mapping, `DiscMap::buildTargetMetadata`, used for the LDX stream output.)

## 3. What already exists that a map could use

- `SourceVideo` is the one door to the video in all the tools (`ld-analyse`, `ld-chroma-decoder`, the stacking tools, `ld-process-*`, `ld-stacker-gui`...), and it already has a **virtual mode**: opened on a `.stack` file (or a session in an LDX file) it
  returns fields that are computed from the sources (`StackedVideo`).
- `StackedVideo` already holds what a mapped view is: a table `FieldPosition { frame, isFirst }` for each field of the timeline, the fields **padded (blank) where no source has a frame**, and the metadata of the result built in memory
  (`readMetadata`). Today the table comes from `origin + offset`. **An offset is a map made of one run.**
- decode-orc's `frame_map` is a list of ranges (`1-11,21-31,12-20`) applied as a lookup table, with `pad_gaps` and `remove_duplicates` as options; its Disc Mapper tool writes the ranges into the project and copies nothing.

## 4. Where a map could be kept

| | A. in the `.stack` session, per source | B. a track of the LDX file | C. a file beside the capture | D. a table in `.tbc.db` |
|---|---|---|---|---|
| What reads it | `StackedVideo` (exists); needs a one-source session to read a mapped capture alone | `LdxCapture` (has `mappedFrom` already) | a new reader in `SourceVideo` | the metadata reader |
| One file per capture to follow | a session file, which already exists for stacking | the LDX file (one file) | yes: a name to follow (`<name>.tbc.map`), but one rule | none |
| Works for a TBC, a CVBS, a Y/C pair | yes (the session names the files) | yes | yes (the chroma file takes the map of its luma) | TBC only (CVBS metadata are `.meta`) |
| Visible to ld-decode and decode-orc | no (they ignore it) | no | no | the schema of ld-decode changes: other tools could break |
| A mapping changes after the capture is changed | the session stamps the captures (size and date) | the track is in the file | a stamp in the map | no check |
| Cost to build | low: extend `offset` to runs | medium | medium | low, but touches the schema |

My reading: **A for the work, B when the capture is in an LDX file, and nothing in the schema (D is out)**. C is the fallback if a mapped capture must be opened by a tool without a session.

## 5. The shape of the map

A list of **runs**: `{ disc frame, capture frame, count, reversed fields? }`, with a run of `capture frame = -1` for padded frames and a flag for unusable ones. A capture without skips is one run (that is today's offset);
a disc with 30 skips is 60 runs. It is also what decode-orc writes (`1-11,21-31,12-20`), so a map can be **exported** to a `frame_map` range string (padding: the gaps between ranges are what `pad_gaps` fills).

What the reader (`MappedVideo`, next to `StackedVideo`, behind `SourceVideo`) has to give: the video fields (a padded one black), the metadata of the result (as `buildTargetMetadata` builds them: the VBI of the map, `pad`, the dropouts of an unmappable frame,
`isMapped`), and the audio ranges of the fields. All three exist as code.

## 6. Legacy, kept clean

- **Reading**: a `<name>_map.tbc` (and its `.db` / `.json` / `.pcm`), or a mapped LDX track, is read exactly as today: nothing to convert.
- **Writing a copy**: `ld-discmap` keeps its way of writing a mapped capture, as an explicit mode (`--copy`, or the output file given as it is today). It stays the way to give a mapped file to ld-decode, decode-orc or any tool that reads a TBC.
- **The new way**: `ld-discmap` writes only the map (into a session, or a track), by default for the Map window. `--maponly` already exists in `ld-discmap` and does the analysis without output: it would write the map.
- **What this removes**: `.mapping-tmp`, `.bak`, the replacement in place, the metadata of the other format left stale (S-12), the `_map` name as the link between a capture and its mapping, and one copy of every captured disc.

## 7. What to look at before building it

1. **VBI in the metadata of the view**: the mapped metadata carry the VBI rewritten from the map. `ld-process-vbi` run on the video of a view would measure the video's own VBI, not the map's. The view must say which it gives (the map's, as the copy does).
2. **Tools that write**: `ld-process-vbi` / `ld-process-vits` given a view would write the metadata of ... what? They should refuse a view, or write to the capture. To decide.
3. **Unmappable frames and padded frames**: a padded field is black in the copy; `ld-chroma-decoder` decodes it as black. The view gives the same field, so the decoding is identical (to compare byte for byte, as in step 1).
4. **Audio**: an analogue audio range per field (`getFirstFieldAudioDataStart/Length`); a CVBS capture has a 24-bit WAV or an LDX track. `AudioReader` / `SourceAudio` would need the map too (two readers, F-02).
5. **Chroma pairs** (`.tbcy` / `.tbcc`, `_chroma.tbc`): the map of the luma is the map of the chroma.
6. **Equal results**: the stack of N views must be identical to the stack of N copies (the sessions test of step 1 gives the way: md5 of the `.tbc` and of the metadata).

## 8. Questions for the person

1. **Where**: A (session, one-source session for a lone capture) with B for LDX, or C (a `.map` beside the capture) for the lone capture, or something else?
2. **What the Map window does by default**: write the map only (and offer "make a copy"), or write the copy as today and offer "map only"?
3. **Does `ld-discmap` change its default?** The command line today writes a copy; to make the map the default is a change of what the tool does with the same arguments. Alternative: `ld-discmap --map-file out.map` for the new way, the copy staying the default until a later version.
4. **Interop with decode-orc**: is exporting a `frame_map` range string worth having (to open the same map there)?
5. **In what order**: first the reader (`MappedVideo`) and the format, with `ld-discmap` writing a map next to the copy so both can be compared, then the tools and the window; the copy removed from the default last.

## 9. Decisions (2026-10-22, the person)

1. **Where**: the **`.stack` session** (a run list per source; an offset is one run) and a **track of an LDX file**; nothing in the schema of `.tbc.db`; no `.map` file for now.
2. **Map window**: writes **the map only**; making a mapped copy is an option.
3. **`ld-discmap` command line**: **the map becomes the default**; the copy is kept as `--copy` (the old behaviour, for ld-decode, decode-orc and old scripts). The same arguments therefore no longer write a `_map.tbc`.
4. **decode-orc**: no export of the map as a `frame_map` range string.

**Remarks added the same day**: the map is an object of its own, kept in a `.stack` but also able to be copied into an LDX file without a session; and it must tell what was **measured** (VBI) from what was **deduced or guessed**. Section 10 is revised for both.

Still to settle before it is built: the format of the map in the session and in the LDX file, and what the command line takes and gives (section 10).

## 10. The design that follows (to approve; revised after the person's remarks)

Two remarks of the person shape it: **the map is an object of its own**, kept in a `.stack` but also able to be copied into an LDX file **without a session**; and a map must say **what was measured and what was deduced**
("VBI or guesswork").

### 10.1 The map as an object

One JSON object, the same wherever it is kept:

```json
{ "ldxMap": 1,
  "capture": { "fields": 189240, "size": 1721740020 },
  "madeBy": "ld-discmap 1.7.0",
  "runs": [ [1000, 1, 4800, "m"], [5800, 0, 3, "p"], [5803, 4801, 20500, "m"], [26303, 25301, 1, "s"] ],
  "unusable": [ 12044 ] }
```

- `runs`: `[first frame of the disc, first frame of the capture (0: padded), how many, basis]`, in the order of the disc, following each other (no gap is left, so the view goes from the first run to the end of the last).
- `capture`: what the map was made for; a reader that finds another capture refuses the map and says so.
- `unusable`: frames kept but not numbered, whose fields read with a dropout over every line (as the copy does now).
- A reader that does not know the object ignores it and reads the capture as it is.

**Where it is kept** (the same object, copied as it is):
- in a **session** (`.stack`, or the `session.stack` attachment of an LDX file), as the `map` member of a source;
- in an **LDX file, on its own**: a track of kind `annotations` (it exists: free text or JSON, "about" a video track by `"track": <id>`), in the namespace `ldx.discmap`, so that it needs no session and travels with the capture it is about. The specification section 8 already
  reserves `ldx.*` for names that it defines. A session of the same file may name that track instead of repeating it.
- **Copying** from one to the other is a copy of the object (`ld-container` gets a command for it, and the tools that read a map take either).

### 10.2 Measured and deduced

`ld-discmap` knows, for each frame, where its number came from (`discmapper.cpp`: `correctVbiFrameNumbersUsingSequenceAnalysis`, `numberPulldownFrames`, `markUnmappableFrames`, `padDiscMap`). The map keeps it, one letter for each run:

| basis | | where the number or the frame comes from |
|---|---|---|
| `m` | **measured** | the number was read from the VBI of the frame itself, confirmed (the two lines agree, S-09) and kept as it was |
| `u` | measured, **unconfirmed** | read from one line only, or the lines disagreed and one was kept (S-09) |
| `s` | **deduced** from the sequence | the VBI gave another number, and the sequence analysis replaced it with what the neighbours say |
| `d` | **deduced** by the pulldown | an NTSC CAV pulldown frame carries no number: it is numbered by the 1-in-5 pattern |
| `g` | **guessed** | a frame with no number that could not be given one: numbered in place or 0, and marked as a dropout (`unusable`) |
| `p` | **padded**, deduced from the gap | no frame of the capture: the size of the gap comes from the numbers on each side of it (and S-11 would bound it) |

What it is for: the stacking tools can leave out what is not measured (an option, `--measured-only`, on `ld-disc-stacker` and `ld-dropout-correct`; the default stays as now); `ld-stacker-gui` and `ld-analyse` can show the share of
each basis and draw the deduced frames differently; a person can see **why** a frame is where it is; and a map made again with better code can be compared with the one before. The map also keeps the totals in its header (`counts`) so that a
tool can say "98.6 % measured" without reading the runs.

### 10.3 The command line

```
ld-discmap [options] input session.stack      the map of input, added to the session (made if there is none; the source is found by its file)
ld-discmap [options] input capture.ldx        the map as a track of the LDX file that holds the capture (refused if input is not in that file)
ld-discmap --copy [options] input output      the mapped capture, as today
ld-discmap --maponly input                    the analysis only, as today; with --report it prints the basis of every run
```
- Without `--copy`, an output that is a `.tbc`, `.cvbs` or `-` is **refused with a message** ("give a .stack session, or --copy for a mapped capture").
- Several captures are mapped one after the other into the same session; the stack is made from it: `ld-disc-stacker session.stack`.
- The Map window writes the maps into its session (and into the LDX file when the source is a track of one), and offers "also make a mapped copy" (`ld-discmap --copy`).

### 10.4 To settle
1. A session with **one** source is a mapped capture on its own (for `ld-analyse`, `ld-chroma-decoder`, `ld-process-*`): should it give that source's own dropouts and VITS in its metadata (not the empty ones of a stack)? I think yes.
2. `ld-process-vbi` / `ld-process-vits` given a session or an LDX with a map: write to the capture, or refuse? I lean to refuse with a message, since the metadata of the view are derived.
3. `ld-disc-stacker a.tbc b.tbc` with no session: unchanged.
4. The letters: are these six bases the right ones, and is "guessed" (`g`) one the person wants kept apart from "deduced"?
5. Does a CAV/CLV disc with **no** VBI at all (early discs) get a map? It would be all `g`/`p`; I would say the tool refuses, as it does now.

### 10.5 Decided (2026-10-23, the person)

1. A session with **one** source gives that source's own dropouts and VITS in its metadata (a mapped capture reads like the capture).
2. `ld-process-vbi` / `ld-process-vits` given a session or an LDX with a map **write into the capture itself** (they measure the original video; the map stays valid). The VBI they measure is the capture's own, not the map's; the view says which it gives.
3. `ld-disc-stacker a.tbc b.tbc` with no session: unchanged.
4. The **six bases** stay (`m`, `u`, `s`, `d`, `g`, `p`), "guessed" kept apart from "deduced".
5. A disc with **no VBI at all**: no map, the tool refuses as it does now.
6. S-09 (confidence of the two VBI lines: the `m` / `u` bases) and S-11 (a gap is padded only up to the frames captured across it + 100, a refused gap is reported) are done **inside the building of the map**, not before.

## 11. Building it: where it stands (2026-10-23)

Steps, each leaving everything working:
1. **The object** (done): `library/tbc/framemap.{h,cpp}` (`FrameMap`, `MapFrame`, `MapBasis`): runs, the basis of each, the unusable places, `reverseFields`, `discType`, the capture it is for (`fields`, `size`), JSON in and out; the capture frame of a run is the frame as the metadata of the capture give frames (`getFirstFieldNumber(k)` / `getSecondFieldNumber(k)`), 0 for a padded one. Test `testframemap`.
2. **ld-discmap makes it** (done, additive): `DiscMap::toFrameMap`, the basis kept by the stages (`m`, `s` from the sequence analysis, `d` for a pulldown frame, `g` for an unmappable one, `p` for padding; `u` waits for S-09), and `ld-discmap --map-file <file>` writes it as JSON (with or without `--maponly`). Checked on a real capture of the person (`D:\decodde\test.tbc`, 5 406 fields, 5 unmappable frames, skips and repeats): the map has 2 624 frames, as the mapped copy that `ld-discmap` wrote of it has, and for each of the 2 624 places the fields of the copy have the same sync confidence, burst level, audio samples and phase as the fields the map says (0 differences).
3. The map in the session (done): `StackSource.map` and `hasMap`, saved and read in the `.stack` file; refused when the file is not of the size the map says; a source with a map is read as the capture, never as a mapped copy beside it.
4. The reader (done): `SourceVideo::openMapped` (the fields of the mapped copy, in its order, blank for a padded frame) and `MappedMetadata::build` (the metadata of the mapped copy; `ld-discmap` builds its own copy metadata with it too, so there is one implementation). `StackedVideo` and `ld-disc-stacker` read a source with a map through them. Checked: the decoded picture of the whole real capture (2 624 frames) through the map is the same byte for byte as through the copy `ld-discmap` wrote; the stack of two mapped sources of a session is the same file and metadata as the stack of the two copies.
5. `ld-discmap input session.stack` (done) puts the map in the session, no capture written (`--copy` as a name for the old way, the default flip, and the Map window are still to do).
6. The LDX track `ldx.discmap`, `ld-container` command, the specification (section 13 and the container format), the tool pages.
7. S-09 (`u`) and S-11 (bound on a gap), then `--measured-only`.

### 11.1 Steps 5 and 6 done (2026-10-23), with the person's change of name

The old way is **`--legacy`** (not `--copy`), and the Map window does not offer it any more.
- `ld-discmap`: the map is the default; the output is a `.stack` session (the map is put in the source of the capture), or `--map-file`; an output that is not a session is refused without `--legacy`. `--legacy` writes the mapped capture as always (checked: the same file as before).
- `ld-stacker-gui`: the Map task (Stack button, Map button of the Info panel) runs `ld-discmap --maponly --map-file`, keeps the map in the session (`StackSource.map`), and the backend reads the source through it (`FrameBackend::openHandle`, `SourceVideo::openMappedFields` with a table shared by the handles of the threads). The choice "keep a copy / overwrite / without mapping" is now "Map / stack without mapping". Follow map on a source that has a map: read through it; off: the capture as it is. `MapDialog` lost the copy, overwrite and LDX-folder code of the Map task (the Update SNR task keeps its own). Tests `testframebackend` (the source loaded with and without the map, a map for another capture refused) and `testmapdialog` (ld-discmap run from the dialog, the map given back, nothing written beside the capture). The windows were not seen by eye.

### 11.2 The map in the metadata of the source (2026-10-23, the person's request and choice)

The person: the Map button of the Info panel is there to **record the map in the metadata of the source** (ldx, json, db), and the stack needs a *save* option that updates the sources. Chosen: JSON member `ldxMap`, SQLite table `disc_map`, LDX through its metadata blocks (they are that JSON; no separate `annotations` track needed), and **every tool reads a capture through its map automatically** (`--ignore-map` to read it as it is).
- Library: `LdDecodeMetaData` keeps the map (`hasDiscMap`, `getDiscMap`, written and read in JSON and SQLite, kept by every rewrite, not touched by the partial writes of ld-process-vbi / ld-process-vits); `LdDecodeMetaData::saveDiscMap` writes it alone (a table for a `.db`, a read-write with `name.bup` for JSON, new metadata for an LDX file); `MappedMetadata::applyEmbeddedMap` turns the metadata of a capture into those of the view and gives the table of fields for `SourceVideo::openMappedFields`.
- Tools: `ld-discmap --save-map` (alone with the input: nothing else is written); `ld-chroma-decoder`, `ld-disc-stacker`, `ld-dropout-correct`, `ld-export-metadata`, `ld-tbc-to-cvbs`, `ld-cvbs-to-tbc`, `ld-analyse`, `ld-stacker-gui` and the sessions (`StackedVideo`) read through the map; `--ignore-map` (in the session: Follow map off) reads the capture as it is. `ld-discmap`, `ld-process-vbi`, `ld-process-vits`, the converters and `ld-container` work on the capture itself.
- `ld-stacker-gui`: the Stack button offers "Map, in the session" / "Map, save in the sources" / "Stack without mapping"; the Map button of the panel offers "Map, save in the source" (default) / "Map, in the session only". A map that is saved in the source is not repeated in the session.
- Checked on the real capture: with the map saved in its `.tbc.db` (the file grows by one table, the backup is `.bup`), `ld-chroma-decoder` gives the same 2 624 decoded frames, byte for byte, as when it decodes the mapped copy that `ld-discmap --legacy` writes (md5 `17ec12f4...`). Tests: `testsqliteread` (JSON and SQLite round trip, kept by rewrites, kept by a partial write, `saveDiscMap`), `testframebackend` (a map in the metadata is followed or not), `testmapdialog` (saved in the source).
- Not done: the analogue audio (`.pcm`) of a capture read through a map is not put in the order of the map (the tools that decode video are not concerned; a tool that played the analogue audio would be); `ld-export-metadata`, `ld-tbc-to-cvbs` and the Y/C pairs read the capture as it is.

### 11.3 Points 2 and 3 of 10.5 done (2026-10-23)
- A session with one source on gives that source's own dropouts and VITS in its metadata (`StackedVideo::readMetadata` clears them only when there are several sources). Test in `testmappedvideo`.
- `ld-process-vbi` and `ld-process-vits` given a `.stack` session with one source on work on the capture of that source (`StackProject::singleCapture`); with several they say which captures to give one by one. They read the metadata of the capture as they are, so a map kept in it is not applied and is kept (partial writes).
