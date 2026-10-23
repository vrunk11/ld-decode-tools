# LDX: container format for decode data

**Version 1.0.** Extension `.ldx`. Profile tag `LDX_PROFILE` = `1`. Media type `video/x-matroska` (DocType `matroska`).

This document is the normative description of the format. A program that follows it can write files that the tools of this repository read, and read what they write. The words MUST, SHOULD and MAY are used as in RFC 2119. Every number in a file is as Matroska has it (big-endian EBML); the samples inside a block are little-endian. All JSON is UTF-8.

The reference implementation is `src/library/ldx` (the container) and `src/library/tbc` (what the tools do with it), and `ld-container` is the tool that makes, shows and checks files. [Examples and test vectors](#22-examples-and-test-vectors) are in `docs/Specifications/examples`.

## 1. Purpose

A capture of a LaserDisc or a tape is several files that must be kept together (the `.tbc` or `.cvbs`, its `.db` or `.json`, the `.pcm` or `.wav` audio, the `.efm` and `.ac3` data, a `.stack` session). LDX puts all of them in **one file, or one pipe**, with:

1. **Everything of a capture in one stream**, so that a tool can send it to the next whatever the source (TBC, CVBS, RF, audio, EFM), and the next tool is told by the stream what it holds.
2. **Lossless compression** (FLAC for 16-bit samples, FFV1 for 10-bit CVBS), without losing the metadata that goes with the data.
3. **Room for what was not planned**: values per field that no database has, annotations written by hand, and the history of what was done. A tool keeps what it does not understand.
4. **Several captures in one file** (the sources of a stack, the captures of one tape) that a stacking session can name by number, so that the session does not depend on where a file is.

Not goals: replacing the CVBS file format (whose sample encodings are reused); being a general video format (a player is not expected to show a decoded picture); editing the samples in place; compatibility with the files of ld-decode or decode-orc (the tools of this repository read and write those files as before, and `ld-container` converts).

## 2. A Matroska profile

A file is a **Matroska file** (RFC 9559) that follows these conventions, so that ffmpeg, VLC and any Matroska library open it. It is recognised by the tag `LDX_PROFILE` (section 4). The DocType is `matroska` (DocTypeVersion 4, DocTypeReadVersion 2).

- **Time scale: 1,000 ns** (one tick is a microsecond). A time in nanoseconds is rounded to a microsecond (section 10).
- **Clusters** are about 30 ms long (a field or two): a block is stamped from the start of its cluster in the 16 bits Matroska gives, which holds for that span. A writer MUST start a new cluster when a block is 30 ms or more after the start of the current one.
- **Blocks** are in `BlockGroup`s. A block of a track holds **one unit** of that track (section 5); the frames of a unit that a codec makes (several FLAC frames) are **EBML-laced** in the one block. A raw unit is one frame with no lacing.
- **Blocks are written in the order of their time, all tracks together.** Blocks of one track MAY have the same time (a unit of metadata about a field, or the two fields of a frame): they are then read in the order they are in the file.
- **Cues** (the index) are written at the end of a finished file, **one cue point for each block**: its time, its track, the position of its cluster (from the data of the segment) and `CueRelativePosition`, the position of the block in the data of that cluster. A writer of a file that is finished SHOULD write the cues; a stream has none (section 15). A reader MUST open a file that has none (section 16).
- **A Void element** of 4,096 bytes follows the list of tracks of a file (not of a stream), so that a track can be added without moving the data. It is optional for a reader to use it.
- **Segment size**: known for a file (written when it is finished), unknown (all ones) for a stream.
- `MuxingApp` and `WritingApp` SHOULD name the program and library that wrote the file (the reference implementation writes `ld-decode-tools ldx`; another implementation writes its own name, for instance `ld-decode-rust`). They carry no meaning for the format: a reader MUST NOT depend on them or refuse a file for their value.

## 3. Tracks

Each stream is a **Matroska track**. Its **id is its track number**: it is never given to another track in the same file, even after the track is removed (the tag `LDX_NEXT_TRACK_ID` holds the next id to give, and only goes up). Everything that refers to a stream (a session, a metadata track, a chroma track) uses the id.

| Kind | Content | Unit of a block | Depth | Matroska track |
|---|---|---|---|---|
| `tbc` | the fields of a TBC capture | one field | 16 | audio, `A_PCM/INT/LIT`, or `A_FLAC`, 1 channel |
| `cvbs` | the frames of a CVBS capture | one frame | 10 | audio, `A_PCM/INT/LIT`, or video, `V_FFV1` |
| `rf` | RF samples | a number of samples | 16 | audio, `A_PCM/INT/LIT`, or `A_FLAC` |
| `pcm` | the analogue audio of the disc | a number of stereo samples | 16 or 24 | audio, `A_PCM/INT/LIT` (or `A_FLAC` for 16) , 2 channels |
| `efm` | EFM t-values | a chunk of bytes | 8 | `D_LDX/EFM` (type 33) |
| `ac3` | AC3 data | a chunk of bytes | 8 | `D_LDX/AC3` (type 33) |
| `metadata` | JSON (section 11) | any | 0 | `D_LDX/METADATA` (type 33) |
| `annotations` | free text or JSON | any | 0 | `D_LDX/ANNOTATIONS` (type 33) |

- Samples that are raw are carried in an **audio track** (`A_PCM/INT/LIT`, with `BitDepth` 16, or 24 for the 24-bit audio): ffmpeg writes and reads 16-bit PCM in Matroska bit for bit, while it refuses raw grey video there. The track still describes itself as in section 4. The audio sampling frequency of such a track is a **stand-in, 48,000 Hz** (FLAC limits the rate and a rate of 14 to 40 MHz is not one for audio); **the real rate is in the parameters** of the track (`sampleRate`).
- A reader that does not know a kind MUST skip the track and MUST copy it on when it writes a file (section 16).
- A file MAY hold any number of tracks of one kind. A `metadata`, `annotations`, `pcm`, `efm` or `ac3` track says which video track it is about with `"track": <id>` in its parameters; without it, it is about the first video track.
- **Unit and time**: a `tbc` unit is a field, stamped with the time of the field; a `cvbs` unit is a frame, stamped with the time of its first field; the units of audio, EFM and AC3 are consecutive pieces, stamped with the time of their first sample (audio) or in proportion to where they are in their data over the time of the video (EFM and AC3, which have no time of their own).

### 3.1 Parameters of the tracks

Parameters are in the description of the track (section 4). A writer MUST give those marked required; a reader MUST ignore those it does not know.

| Kind | Parameter | Meaning |
|---|---|---|
| `tbc`, `cvbs` | `system` (required) | `"PAL"`, `"NTSC"` or `"PAL-M"` |
| | `sampleRate` | sample rate of the video, in Hz (4 times the subcarrier for the standard captures) |
| | `fieldWidth`, `fieldHeight` (required for `tbc`) | samples of a line, and lines of a field, as in the metadata |
| `tbc`, `cvbs` | `role` | `"chroma"` for the chroma file of a Y/C capture (section 7.3) |
| | `track` | for a chroma track: the id of the luma track |
| `tbc`, `cvbs` | `mappedFrom` | for a track that is the mapping of another capture of the file (section 7.4): the id of the video track it is the mapping of |
| `cvbs` | `lineWidth` (required) | samples of a line of the frame |
| | `frameSamples` (required) | samples of a frame: 477,750 (NTSC), 709,379 (PAL), 477,225 (PAL-M) |
| | `sampleEncoding` | the CVBS encoding the frames had in their file (`CVBS_U10_4FSC`...), to give the file back as it was |
| | `padValue` | the blanking level, 10 bits, with which the end of an FFV1 image is filled |
| `pcm` | `sampleRate` (required) | real sample rate |
| | `channels` | 2 |
| | `bits` | 24 for the audio of a CVBS capture; absent or 16 otherwise |
| | `source` | `"efm"` for the digital audio decoded from the EFM of the capture (44,100 Hz, 16-bit stereo); absent for the analogue audio. A reader that wants the analogue audio MUST NOT take a track whose source is `efm`. |
| | `content` | `"dts"` when the decoded EFM audio holds DTS in PCM (informative) |
| `metadata` | `namespace`, `format`, `track` | `"ldx.tbcmetadata"`, `"ld-decode-json"`, the video track it is about |

## 4. Tags

All the tags are `SimpleTag`s with a name and a string.

| Tag | Target | Value |
|---|---|---|
| `LDX_PROFILE` | the segment | the version of this document as an integer: `1` |
| `LDX_HISTORY` | the segment | the history, a compact JSON array (section 12) |
| `LDX_NEXT_TRACK_ID` | the segment | the next track id to give, an integer (it only goes up) |
| `LDX_TRACK` | a track (by its `TagTrackUID`) | the description of the track, a compact JSON object (below) |

The description of a track, whatever its codec, is:

```
{ "ldx": 1, "kind": "tbc", "codec": "raw", "depth": 16, "parameters": { ... } }
```

`kind` is one of the names of section 3, `codec` is `raw`, `flac` or `ffv1`, `depth` is the depth of the table of section 3 (a missing depth is 16), `parameters` is an object (section 3.1). A raw track has the same JSON as its `CodecPrivate`, as the first files had; a reader MUST read the tag, and MAY fall back on the `CodecPrivate` of a raw track when the tag is missing. The `CodecPrivate` of a FLAC or FFV1 track is what its decoder needs (below).

The **first tags** of a file or stream are written before the data. A tool that adds to a finished file (section 17) writes tags at the end: **the last tags of the file are the current ones** (a reader reads all of them and the later value of a tag replaces an earlier one).

## 5. Units

A unit is the content of one block *after the codec is undone*. Writers write units; readers return units; the tools never see codec frames.

### 5.1 `tbc`: a field

The samples of a field in order, `fieldWidth * fieldHeight` unsigned 16-bit little-endian words (the line after line of a TBC file). Fields are written in the order of the capture and the two fields of a frame follow each other. A unit is the same bytes as that part of the `.tbc` file.

### 5.2 `cvbs`: a frame

The samples of a frame of a CVBS file, as **10-bit values in 16-bit little-endian words** (0 to 1,023), `frameSamples` of them: the `CVBS_U10` encoding **whatever the encoding of the file the frame comes from**. A frame of another encoding is converted by taking its TBC-domain value (16 bits, by the rules of the CVBS specification for that encoding) and dividing by 64 (shifting right by 6); `unpack` converts back (shifting left by 6 and applying the inverse of the encoding), which gives the file back byte for byte for a frame that came from a file.

### 5.3 `pcm`: audio

A unit is a number of stereo samples, left then right for each sample: **16-bit signed little-endian** (4 bytes a sample) or, with `"bits": 24`, **24-bit signed little-endian** (6 bytes a sample, as in the WAV file `name_audio_0.wav` of decode-orc). Units follow each other without a gap: the first sample of a block is `time * sampleRate`, rounded. The 16-bit audio of a TBC capture is the `.pcm` file of ld-decode.

A capture can thus have two `pcm` tracks about it: the analogue audio and the digital audio of its EFM (`ld-process-efm` and `efm-decoder-audio` write the latter into the file, section 19).

### 5.4 `efm`, `ac3`: bytes

Chunks of the file they come from (64 KiB, the last one shorter). **Put end to end, the chunks are the file.** Their time is spread over the time of the video, in proportion to where a chunk is in the file.

### 5.5 `rf`

Samples of the RF capture, 16-bit little-endian, in pieces of any size, the rate in `sampleRate`.

## 6. Codecs

| Depth | Allowed |
|---|---|
| 16 bits (`tbc`, `rf`, `pcm`) | raw, FLAC |
| 10 bits (`cvbs`) | raw, FFV1 |
| 24 bits (`pcm`), 8 bits, text | raw |

- **Raw is valid for every track and every reader MUST read it.** In a pipe, writers use raw.
- **Decoding returns exactly the samples that were encoded.** A reader that does not have a codec MUST refuse the track with a message that names the codec; it MUST NOT read data it cannot interpret.
- **A codec is the choice of the writer**; a tool may change it for a file that is written (`ld-container recompress`) without touching the rest.

### 6.1 FLAC (`"codec": "flac"`)

- Matroska codec `A_FLAC`. `CodecPrivate` is `fLaC` followed by the 34-byte STREAMINFO block of the encoder (as Matroska requires).
- A block of the file is **one unit** coded as the **FLAC frames** the encoder gives, **EBML-laced** (a single frame is not laced). Frames are of **32,768 samples** (the last one shorter), **a unit is at most 8 frames, 262,144 samples** per channel; a longer unit MUST be refused by a writer. (A field of a TBC is 239,330 samples.)
- **Unsigned samples** (`tbc`): the encoder is given the samples made signed by subtracting 32,768 (flipping the top bit), and the decoder's output is made unsigned again by adding it. A FLAC decoder that does not know this shows the samples with the top bit flipped.
- **Stereo** (`pcm`) is two FLAC channels together, not one channel of alternating samples.
- Compression level 8. The sample rate in the FLAC data and in Matroska is the stand-in 48,000 Hz.
- Each unit is coded with an encoder of its own (a short frame is only allowed as the last of a stream), so a block can be decoded alone.

### 6.2 FFV1 (`"codec": "ffv1"`)

- Matroska codec `V_FFV1`, a video track of `lineWidth` x `height` pixels, with `height = ceil(frameSamples / lineWidth)`. `CodecPrivate` is the FFV1 configuration record of the encoder.
- The pixel format is **10-bit grey, little-endian** (`gray10le`): a frame of `frameSamples` words is **laid out as an image row after row**; when the last row is not whole, **it is filled up with `padValue`** (a PAL frame, 709,379 samples, is 625 lines and 4 samples: an image of 1,135 x 626 whose last row has 4 samples and 1,131 of padding). A decoder cuts the frame to `frameSamples` again. NTSC (910 x 525) and PAL-M frames fill their images.
- FFV1 version 3, Golomb-Rice off (range coder, `coder` 1), `context` 1, 4 slices with CRC, **every frame a key frame** (a block decodes alone).
- A player that knows Matroska shows the track as video of that size.

## 7. Video captures

### 7.1 One capture

A capture is a video track (`tbc` or `cvbs`), the `metadata` track about it, and as many of the `pcm`, `efm` and `ac3` tracks about it as the capture has.

### 7.2 Which capture a name gives

See section 14.

### 7.3 Y/C captures

A Y/C capture (a luma file and a chroma file: `name.tbc` + `name_chroma.tbc`, or `name.tbcy` + `name.tbcc`) is **two `tbc` tracks**: the luma, as for any capture, and a **chroma track** with `"role": "chroma"` and `"track": <id of the luma track>` in its parameters. The chroma has no metadata track: the metadata of the luma are those of the pair. The chroma track has as many units as the luma and the same time for each. A reader that does not know the role reads it as another capture; the tools list only the tracks that are not chroma as the captures of a file. A CVBS Y/C pair (`name.cvbsy` and `name.cvbsc` of decode-orc, which share `name.meta`, the dropouts and the companion metadata) is the same with `cvbs` tracks: the luma `cvbs` track, and a chroma `cvbs` track with `"role": "chroma"` and `"track": <the id of the luma track>`, each with the parameters and the codec of section 3.1 and 6. `unpack` names them `<name>.<id of the luma>.cvbsy` and `.cvbsc`.

### 7.4 Mapped captures

A capture that was **mapped** (ld-discmap: the frames in the order of the disc, the repeated ones removed and the missing ones padded) can be kept in the file beside the capture it was made from, as a video track of its own with `"mappedFrom": <id of the original track>` in its parameters. The original is a video track of the file; a `mappedFrom` that names a track that is not there, or that is not a video track, is ignored and the track is a capture like any other. The metadata of the mapped track are those of the mapping (and say `isMapped`), and a mapped Y/C capture is two tracks again, the chroma with the `role` and `track` of section 7.3 about the mapped luma. A reader that does not know the parameter reads the mapped track as another capture, which it is. `ld-container add --mapped-from <id>` writes such a track, and ld-stacker-gui makes one with it. The tools find the mapped track of a capture from the original, and the original from the mapped track; a name with no track number gives the first capture that is not a mapping.

## 8. Metadata

### 8.1 The format

The metadata of a capture are **ld-decode metadata as JSON** (the content of a `.tbc.json`), on the `metadata` track about the capture, namespace `ldx.tbcmetadata`, format `ld-decode-json`:

```
{ "videoParameters": { ... }, "pcmAudioParameters": { ... }, "fields": [ { ... }, ... ] }
```

- `videoParameters` members: `system`, `fieldWidth`, `fieldHeight`, `sampleRate`, `activeVideoStart`, `activeVideoEnd`, `colourBurstStart`, `colourBurstEnd`, `black16bIre`, `white16bIre`, `blanking16bIre`, `isMapped`, `isSubcarrierLocked`, `isWidescreen`, `isSourcePal`, `numberOfSequentialFields`, `tapeFormat`, `gitBranch`, `gitCommit`.
- `pcmAudioParameters` members: `sampleRate`, `bits`, `isSigned`, `isLittleEndian`.
- a field has `seqNo` (from 1, in the order of the video), `isFirstField`, `syncConf`, `medianBurstIRE`, `fieldPhaseID`, `audioSamples`, `diskLoc`, `fileLoc`, `efmTValues`, `pad`, `decodeFaults`, `vbi`, `vitc`, `ntsc`, `cc`, `vitsMetrics`, `dropOuts`.
- **`numberOfSequentialFields` MUST equal the number of members of `fields`** in a block that has fields.

The members are those the tools of this repository read and write; the JSON of a `.tbc.json` of ld-decode is valid. A reader MUST ignore members it does not know and a tool that rewrites metadata MUST keep them.

### 8.2 Blocks

Three kinds of block can be on a metadata track. A reader tells them by their content:

1. **A whole set**: a block with `videoParameters` and `fields`. This is what `ld-container pack` writes for a file.
2. **The parameters alone**: a block with `videoParameters` and **no fields** (`numberOfSequentialFields` 0), with the `pcmAudioParameters`. The fields come in blocks of the third kind.
3. **One field**: a block that is a single JSON object as a member of `fields` is (it has `seqNo`, and no `videoParameters`).

### 8.3 The current metadata

A reader makes the current metadata of a capture by reading the blocks of the track in the order they are in the file. **A block with `videoParameters` (kinds 1 and 2) starts the metadata again**: it is the base, with its `fields` if it has any, and what came before it no longer counts. **A block of one field (kind 3) replaces the field with its `seqNo` in the base, or adds it.** `numberOfSequentialFields` is the number of fields that results. A block never removes a field.

An earlier block stays in the file until the file is written again. A tool that changes metadata of a finished file adds a block stamped after everything in the file (section 17); a tool in a pipe sends the changed block of a field before the video of that field (section 15).

### 8.4 Other metadata

A track of kind `metadata` or `annotations` that is not `ldx.tbcmetadata` declares its own **namespace** in its parameters: `ldx.*` is reserved for names defined by this specification, anything else is chosen by a tool in reverse domain order (`com.example.mytool`). Each block says what it applies to (a time or a range of time) and gives its values; of two blocks of one track, target and key, the last wins, and a block MAY carry `"deleted": true` to cancel an earlier one. A tool MUST NOT rewrite a block it did not write.

### 8.5 The map of a capture

The metadata of a capture MAY keep the **map** of the capture (the object described in section 13, under "The map of a source"): the frames of the capture put in the order of the disc, with where each number comes from. It is kept where the metadata are: in the JSON, as the member `"ldxMap"` of the object (so in the metadata blocks of an LDX file, which are that JSON); in a SQLite file, as a table `disc_map (capture_id INTEGER PRIMARY KEY, map TEXT NOT NULL)` that only a file with a map has, the map being the JSON text of the object. A reader that does not know the map ignores it (an unknown member, an unknown table) and reads the capture as it is.

A tool that reads the video of a capture MUST read it through the map when the metadata keep one, unless it was asked to read the capture as it is: the fields of the capture are then seen in the order of the map, with the metadata of the mapped copy of the capture, as for a source with a map in a session (section 13). A tool that rewrites the metadata MUST keep the map; one that works on the capture itself (measuring the VBI or the test signals) reads and writes the metadata as they are.

### 8.6 Decoding parameters

The metadata of a capture MAY keep how it was chosen to be decoded to colour, in the same places as the map (8.5): the member `"ldxDecoding"` of the JSON (so in the metadata blocks of an LDX file), and in a SQLite file a table `decoding (capture_id INTEGER PRIMARY KEY, parameters TEXT NOT NULL)` that only a file with parameters has, the text being the JSON object:

```json
{ "ldxDecoding": 1, "decoder": "transform3d", "madeBy": "ld-analyse",
  "settings": { "chromaGain": 1.0, "chromaPhase": 0.0, "lumaNR": 0.0, "simplePAL": 0 } }
```

`decoder` is a name of ld-chroma-decoder `--decoder` (`pal2d`, `transform2d`, `transform3d`, `ntsc1d`, `ntsc2d`, `ntsc3d`, `ntsc3dnoadapt`, `mono`). `settings` are numbers by name (`chromaGain`, `chromaPhase`, `lumaNR`, `chromaNR`, `adaptThreshold`, `chromaWeight`, and `phaseCompensation` and `simplePAL` as 0 or 1); a reader ignores the names it does not know. A tool that decodes the capture uses them as its defaults, and what the person says (an option, a setting in a window) has the last word. A reader that does not know them ignores them, and a tool that rewrites the metadata MUST keep them. They are not for a stack of several sources.

## 9. Attachments

An LDX file MAY hold files as Matroska attachments (name, media type, data). Defined: `session.stack` (section 13). A reader MUST ignore attachments it does not know and a tool that writes a file MUST copy them on.

## 10. Time

All blocks have a time in the time scale of the file (1 µs). The timeline is that of the first field of the capture. A field is `1/50` s (PAL) or `1001/60000` s (NTSC, PAL-M) after the one before: the time of field *n* (from 0) is `n * 20,000,000 ns` for PAL and `n * 16,683,333 ns` for NTSC and PAL-M (`n * 1,000,000,000 * 1001 / 60000`), rounded to a microsecond. A frame of CVBS has the time of its first field. Units of different tracks are lined up by their time, not by a count.

## 11. Streams of tracks that a tool does not know

A tool that reads a file or a stream and writes another **MUST copy the tracks, blocks, attachments and tags it does not understand, unchanged**, and add its own. A tool MAY change a track it knows how to (recompress, remove) and MUST say so in the history.

## 12. History

The history is a JSON array in the tag `LDX_HISTORY`. Every tool that changes a capture adds **one entry** (an object), after the earlier ones:

| member | |
|---|---|
| `tool` | name of the program (`ld-dropout-correct`) |
| `version` | the version of the program, as it names its versions: a release number or a commit (free text; a reader does not interpret it) |
| `command` | what it did: `pack`, `process`, `metadata`, `recompress`, `add`, `remove`, `stack`... |
| `time` | UTC, ISO 8601 |
| `detail` | a sentence for a person (optional) |
| `arguments` | the arguments of the program as given (optional) |

A tool MAY add members of its own (the files it read, the options it took, what it counted); a reader ignores those it does not know. A tool MUST NOT remove or change an entry of another. A stream keeps those that came in, and the tool adds its entry to the header of the stream it sends on. The history is a record, not a lock: nothing in a file depends on it.

## 13. Sessions (`session.stack`)

A stacking session can be an attachment named `session.stack` (media type `application/json`), in the format of the `.stack` files of `ld-stacker-gui` and `ld-disc-stacker`. Each of its sources names its capture in one of three ways:

| Source | Meaning |
|---|---|
| `"track": 2` | the track 2 of the same LDX file |
| `"file": "other.ldx", "track": 2` | the track 2 of another LDX file (a link) |
| `"file": "capture.tbc"` | a file outside this one (a link): a TBC, a CVBS, an LDX file or a `.stack` file |

The path of a link is relative to the `.ldx` file. A tool that writes a session into a file MAY embed the sources that are files (copying each into a track of its own with its metadata and audio) or leave them as links; both are valid. A tool that removes a track MUST take the sources that name it out of the session and say so; links are not touched.

**The map of a source.** A source MAY have a member `map`: the map of its capture, which says in what order the frames of the capture are put on the disc and where each number comes from. The source is then **the capture seen through the map**, and not the capture: a reader gives the frames in the order of the map, with the metadata that the mapped copy of the capture would have (the field records of the frames, `isMapped` set, the number of the disc in the VBI of the first field of each frame, a padded frame as two blank fields with `pad`, the dropouts of an unusable frame over every line), without the copy being written. A mapped copy of the capture that is beside it (`name_map.tbc`) is not looked for. The object:

```json
{ "ldxMap": 1,
  "capture": { "fields": 5406, "size": 2587635960 },
  "madeBy": "ld-discmap",
  "reverseFields": false,
  "discType": "CLV",
  "runs": [ [0, 8, 1, "g"], [1, 29, 17, "m"], [18, 103, 39, "m"], [253, 338, 2366, "m"] ],
  "unusable": [1, 2, 4],
  "counts": { "frames": 2624, "m": 2619, "g": 5 } }
```

- `ldxMap` is the version of the object (1). A reader that does not know the version, or finds the object not good, MUST say so and read the source as the capture.
- `capture` says what the map was made for (the fields and the size in bytes of the video file): a reader that finds another capture MUST NOT use the map.
- `runs` are the frames of the view in order, each `[first disc number, first frame of the capture, how many, basis]`: the numbers of the disc and the frames of the capture both go up by one in a run; a frame of the capture is a frame as the metadata of the capture give frames (the fields `getFirstFieldNumber` and `getSecondFieldNumber` of the frame), numbered from 1, and 0 means that there is none: a padded frame (the basis is then `p`, and only then). The frames of a run all have its basis.
- `unusable` are the places in the view (from 1) of frames that are kept but could not be numbered: their fields are read with a dropout over every line.
- `reverseFields` says that the capture was read with the second field first; `discType` is `CAV` or `CLV`, which the VBI words of the numbers are written from.
- `basis` says where the number or the place of the frames comes from, so that a tool can leave out what was not measured: `m` read from the VBI of the frame, the two lines that carry it agreeing; `u` read, but from one line only, or the lines disagreeing; `s` replaced by what the numbers around it say; `d` an NTSC CAV pulldown frame numbered by the 1-in-5 pattern; `g` a frame with no number that could not be given one; `p` padding, whose place comes from the gap between the numbers around it.
- `counts` is the number of frames and of each basis (optional, for a tool that wants the share of the frames that were measured).

An offset of the source is added to the numbers of the map as to the origin of a capture.

**How a session is read.** The sources that are on are lined up on a timeline by the numbers of the disc that their VBI gives (the picture number of a CAV disc, the time of a CLV one, in frames); the origin of a source is the number of its first frame, the one that most of its first frames agree on (a picture number of 0 is a number, and the origin can be 0 or negative, when the capture begins with frames that have no number before the first picture of the disc). The source whose origin is the earliest begins the timeline, every other source begins where its origin puts it, and its `offset` (a number of frames, which may be negative) is added to that. A source whose VBI gives no number (for instance a capture that was not mapped and whose first frames cannot be read) begins at the number 0 of the disc, plus its offset, so that it is at the start of the timeline while the sources that have numbers are where these put them. The timeline goes from the earliest frame of any source to the latest of any: a frame that no source has is a padded, blank frame. The metadata of a session have the `fieldPhaseID` of its fields put in sequence again, as `ld-disc-stacker` leaves them: from the first field that is not padded, 1 to 4 for NTSC and PAL-M and 1 to 8 for PAL. A reader MUST NOT make or change a mapping: a source whose `followMap` is true (the default, when the member is absent) and that has a mapping that exists (a file beside it with `_map` in its name, or the track of the same file whose `mappedFrom` parameter names it, section 7.4) is read from that mapping unless the metadata of the source say it is mapped already, and a source that has none is read as it is. A source whose `followMap` is false is read as the capture it was made from: if its file or track is a mapping, the capture that it is the mapping of. The member is a boolean of the source, beside `enabled` and `offset`; a writer need only write it when it is false. Every tool that reads a session (ld-analyse, ld-chroma-decoder, ld-disc-stacker) takes the mapping into account in this way.

**A session with one source on** is that source: the metadata a tool reads from it are those of the source (its dropouts and its VITS measurements, and the map if it has one applied), and not those of a stack, which has neither. A tool that works on a capture itself (measuring the VBI or the test signals) given a session with one source on works on the capture of that source; given several, it says which captures to give one by one.

**The streams copied into the stack.** The member `copyStreams` of `stacking` MAY list the streams of the sources (EFM, AC3, RF, audio of a CVBS capture) that a tool copies into the LDX file it writes from the session: an array of `{"source": <id of the source>, "track": <the track in the file of the source>}`. When it is not there the tool copies the streams of the first source; an empty array means none. The copy is block by block, with the times the blocks have in their source (the stack does not realign them), and the history of the output says what was copied.

**The offset between the video and a copied stream is recorded, so that it can be corrected.** A stream that is copied keeps the times it has in its source, and the video of the output is not that of the source (it begins at another frame, skips, or has padded frames), so the track carries the parameter `alignment`: `{"unit": "frame", "segments": [[output frame, source frame, count], ...]}`. In a segment the frame of the video of the output (from 1) and the frame of the capture the stream belongs to both go up by one; a source frame of 0 means that the source has no frame there. A source that is not shifted is the one segment `[1, 1, n]`. A reader finds the part of the stream that goes with the frame *n* of the video by the segment that holds *n*: the stream time is the one the same position has in the capture of the source, that is (source frame - 1) times the period of the video from the start of the stream in its own file. The same holds for a capture seen through a map (section 8.5): the map gives, for each frame of the view, the frame of the capture, which is all that is needed to find the audio or the streams of the capture (the analogue audio of a stack is written from the fields, so it is aligned by construction).

**What was measured.** A session MAY keep what was measured of it, in the optional array `measures`, so that it need not be measured again and so that a tool can use it as metadata. Each member has a `key`, and for each of `white` and `black` (the SNR of the white flag and the PSNR of the black level, in dB) and `residual` (the total length of the dropouts that the stack is left with, in samples) an array with a value for each frame along the timeline (for a measure of one source, along its own frames); a frame that has no value is `null`. The measures that are about the stack are under the key `stack` or `stack+dropouts` (the second has `residual` too), followed by, for each source that is on, `|<name of the capture read>#<track>|<size in bytes>@<offset>`, and then `|<mode>,<smart threshold>,<diff dod>,<passthrough>,<reverse fields>` as the options are in the session (the capture read is the one that `followMap` says; the name is that of the file, with no folder). The measures of a source are under `source|<name>#<track>|<size>`. A reader MUST use a measure only if its key is the one it would make for the session it reads, and MUST ignore the others, so that a measure of other captures, offsets or options is never taken for this one; a reader that does not know `measures` ignores it. The metadata of a session that a tool reads (`ld-analyse`, `ld-chroma-decoder`) have the `white` and `black` of the measure of the stack, if it is good, as the VITS SNR of the fields of each frame. A tool that stacks to a file (`ld-disc-stacker`) MAY line the sources up with its first source and the `offset` only, and then the stack it writes is as long as that source.

## 14. Names, and which capture a tool reads

The tools take an LDX file where they take a `.tbc`, `.cvbs` or `.stack`:

| Name | What is read |
|---|---|
| `capture.ldx` | the stack of the session the file keeps; if it keeps none, its first video track (`tbc` or `cvbs`) that is not a chroma track |
| `capture.ldx#3` | the track 3, with the metadata, audio, EFM and AC3 tracks that are about it |
| `-` | an LDX stream on the standard input (section 15) |

The `#` and the number are only a part of the name the tool is given: nothing is written. A source of a session that is a track of another file is the same name (`"file": "other.ldx", "track": 3` is `other.ldx#3`). The parameters of the metadata of a session are those of its first source that is on, and its fields are those of the first source that has each frame of the timeline (section 13). The default track of a file is the first, by id.

## 15. Streams (pipes)

An LDX stream is an LDX file written to something that cannot be seeked: **one producer to one consumer**, everything in one stream.

**A producer writes**: the EBML header, a segment **with unknown size**, the info, the tracks, the tags (with the history up to now) and the attachments, then the clusters as the blocks come, in the order of their time. **Nothing is written again**: no cues and no padding, and the tags are only at the start. The **metadata of the video are sent before the video they describe**: either as one set (section 8.2, kind 1), or as the parameters (kind 2) followed, for each field, by the block of that field (kind 3) **stamped with the time of the field and placed just before its video**. A producer that has no metadata yet sends the video alone, and the consumer takes the video system from the track and the defaults of the system. Codecs are as for a file: raw in a pipe is the usual choice, a consumer decodes FLAC and FFV1.

**A stream is an LDX file with no index**: a stream written to a file (`ld-container pack capture.tbc -o - > capture.ldx`) is a file like any other, and a program that opens it makes the index by reading it once (section 16); `ld-container recompress` writes it with its index. **A consumer that reads a pipe (`-`) reads in order**: it cannot seek or read a block again, and there is no index to be had. It keeps a window of what has come in. A consumer that needs the metadata of all the fields before it begins (stacking, dropout correction across sources) MUST refuse a stream that sends them field by field and say why; a consumer that works field by field (decoding, display) takes the metadata of a field from its block when it has the field, and the defaults when there is none.

**A producer SHOULD declare in the tracks, at the start, every track it may send during the stream**, even one that holds nothing for a while or never does (the EFM track of a disc whose EFM is not locked yet, the audio track): the tracks are listed once, in the header, and a consumer takes the blocks of the tracks it was told of, so the blocks of a track that was not declared are not seen. A consumer that is shown a track with no blocks yet waits for them.

**A tool in the middle** passes along what it does not know (section 11), may replace the metadata of a field by sending a later block for the same `seqNo` before the video of that field, and adds its entry to the history of the header it sends. It SHOULD hold back only the field it is working on, not the stream.

**Standard input and output** are binary. A tool that writes a stream on the standard output sends its messages to the standard error. A tool that takes raw data on the standard input as well tells a stream from it by how it begins (the bytes `1A 45 DF A3`).

## 16. Reading rules

- A track **kind** that is unknown: skip the track. A **codec** that is unknown: refuse the track and name it. A **parameter** that is unknown: ignore it.
- A file with no `LDX_PROFILE`, or with a profile that is not an integer, is not an LDX file. A reader MUST refuse a **major version** it does not know: a change of this specification that makes an older reader wrong is a new profile number.
- A cue is for one block; if there are several cues for blocks of one track at the same time, they are in the order of the blocks in the file (sorted by time, then by position).
- **A file with no cues is read like one that has them.** A reader that needs the index (random access, a track by its number, `verify`) MUST make it by reading the file once: the headers of the clusters, of their block groups and of their blocks give the time, the track and the position of every block (the data of the blocks are not read). A cluster whose size is not known ends where the next one begins; the segment ends where the file does. The result is the index the cues would have given.

## 17. Changing a file

**Adding blocks to a finished file in place** (`openAppend`): the new clusters go where the cues were; the cues are written again, with the old ones and the new; the size of the segment is corrected; the file is cut after the cues; the tags written at the end carry the history with what was added. Nothing else is moved. A file that has no cues (a stream saved as it came) is added to where its data end, and is then a file with cues. This is how the tools add the new metadata of a capture (a block of the `metadata` track, stamped after everything in the file, section 8.3).

**Adding or removing a track** writes the file again (the other tracks' blocks are copied and merged by time, with the new ones). The new track gets the next free id; a removed id is never given again; a metadata, annotation, audio or chroma track that is about a removed track goes with it.

## 18. Conformance

A **writer** MUST: write `LDX_PROFILE`, `LDX_HISTORY`, `LDX_NEXT_TRACK_ID` and `LDX_TRACK` for every track; use the time scale and cluster span of section 2; write cues for a file and none for a stream; keep metadata blocks of a track in the order they were produced; never write a unit of a codec longer than its limit; and put the metadata of a field before its video in a stream.

A **reader** MUST: read raw tracks; read all the tags and keep the last value; read the current metadata as section 8.3; skip what it does not know; refuse a codec it lacks by name; and, for a file, find a block through the cues or, when there are none, through the index it makes (section 16).

## 19. Putting the output of a tool in a file

The tools that make audio or data from a capture can leave it in the LDX file as a track of its own: `ld-process-efm capture.efm capture.ldx` and `efm-decoder-audio capture.d24 capture.ldx` add the digital audio of the EFM as a `pcm` track with `"source": "efm"`, and `ld-container addtrack capture.ldx --kind ac3 --input capture.ac3` adds AC3 data (or EFM t-values, or annotations). The track goes with the video track named by `--about` (the first by default), the LDX file is written again with the track and an entry of the history, and `ld-container wav capture.ldx [--source efm]` gives a track back as a WAV file.

## 20. Names of the tools' files

`ld-container unpack` writes the separate files of a capture as `<name>.<id>.<suffix>`: `.tbc`, `.cvbs` (with its `.meta` and its companion JSON), `.pcm`, `.efm`, `.ac3`, `.chroma.tbc`, `_audio_0.wav` (for the 24-bit audio of a CVBS capture), `.tbc.json` (the current metadata), and the attachments by their names.

A tool that opens a CVBS capture whose metadata are the core `<name>.meta` alone measures the phase of each field from the colour burst, which reads a part of every frame; it keeps what it measured in `<name>.cvbs.phases` beside the data file (a text line that names the size and the date of the data file and the number of fields, then a byte for each field), and uses it while the data file is as it was. The file can be deleted at any time; nothing else depends on it.
## 21. Libraries

The reference implementation uses **libebml and libmatroska** (LGPL-2.1-or-later) for the container and **libavcodec** (ffmpeg) only to encode and decode FLAC and FFV1. A program may use any other library (a Matroska muxer, ffmpeg itself) as long as the conventions above hold.

## 22. Examples and test vectors

`docs/Specifications/examples/` holds small files that cover the format and the expected content of each, with SHA-256 sums, so that an independent implementation can check that it reads what the reference writes and that its own files are read by the reference tools (`ld-container info`, `verify` and `unpack`). See its `README.md`.
