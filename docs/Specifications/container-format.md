# LDX: container format for decode data

**Version 1.0, ratified by the author of the fork. Nothing is implemented yet.**
Extension: **`.ldx`**. Profile tag: `LDX_PROFILE` = `1`.

The format is for the users of this fork. Compatibility with ld-decode or decode-orc is not a goal of version 1.0: ld-decode support will come from a custom build or fork. If a common container is wanted later, it is a new version of the profile (section 11).
What is left open (section 12) does not change what a file contains: it is about the first implementation.

## 1. Why

Today a decode is several files that must be kept together: the `.tbc` or `.cvbs`, its `.db` or `.json`, the `.efm`, `.ac3` and `.pcm` files, a `.stack` session. Tools read them one by one, and each pair of tools has to agree on the formats between them.

This format has two uses, and everything else follows from them:

1. **One pipe, or one file, with everything in it.** A producer such as ld-decode can pipe to ld-analyse or to any other tool, whatever the source (TBC, CVBS, RF, audio, EFM), and the tools do not have to care which format the data was in.
2. **Lossless compression**, to save space, without losing the metadata that goes with the data.

Because the format is meant to last, it must also allow tools to **add data it did not plan for** (per-frame values that are in no existing database or JSON, annotations written by hand), and keep what a tool does not understand.

### Not goals

- Replacing the CVBS file format specification, whose sample encodings it reuses.
- A general video format. Players are not expected to show a decoded picture.
- Editing the stored data in place.

## 2. Principle: a Matroska profile

A file is a **Matroska file** (RFC 9559) that follows the conventions below. Reasons, from what was compared (NUT, a custom block format, SQLite, ZIP):

- It can be written to a pipe (a segment and its clusters may have an unknown size), and read by ffmpeg and VLC.
- FLAC and FFV1, the two codecs wanted, are normal Matroska codecs.
- Several tracks, attachments, tags and a per-block side data mechanism already exist.

What it costs: a library to read and write it (libebml and libmatroska, or ffmpeg), and a weak point: adding data about an old timestamp after the file is written (section 9).

A reader recognises the profile by the tag `LDX_PROFILE` (the version of this document, as an integer) in the Segment tags. The DocType stays `matroska`, so that generic players still open the file.

## 3. Streams

Each stream is a **Matroska track**. A track has a *kind*, a *bit depth* (the useful bits of a sample), a *codec*, and parameters. They are written in the track's `CodecPrivate` as a JSON object (UTF-8), so that a tool that does not know the track can still list it.

| Kind | Content | Unit of a block | Depth |
|---|---|---|---|
| `tbc` | the fields of a TBC | one field | 16 |
| `cvbs` | the fields of a CVBS capture, in the 10-bit domain of the CVBS specification | one field | 10 |
| `rf` | RF samples | a fixed number of samples | 16 |
| `pcm` | the analogue audio of the disc | a fixed number of samples | 16 |
| `efm` | EFM t-values | one frame (variable size) | 8 |
| `ac3` | AC3 data | one frame (variable size) | 8 |
| `metadata` | JSON, see section 6 | any | text |
| `annotations` | free text or JSON written by a person or a tool, see section 6 | any | text |

A block carries the data of one unit and is stamped with the time of the unit (the field rate for fields, in nanoseconds). Fields of a frame follow each other, first field first. A kind that is not in this table is ignored by a reader that does not know it, and must be copied on (section 8).

The parameters of a track are those the tools already use: video system, sample rate, field width and height, the position of the active region, the black, white and blanking levels, the order of the fields. They are the fields of the existing `videoParameters` where they exist.

## 4. Codecs

| Depth | Codecs allowed | Matroska track |
|---|---|---|
| 16 bits (`tbc`, `rf`, `pcm`) | raw, FLAC | raw: audio track `A_PCM/INT/LIT`, 16 bits, one channel. FLAC: `A_FLAC` |
| 10 bits (`cvbs`) | raw, FFV1 (`gray10le`) | raw: audio track `A_PCM/INT/LIT`, 16-bit words holding values 0 to 1023. FFV1: video track |
| 8 bits (`efm`, `ac3`) | raw | binary track |
| text | raw (JSON, UTF-8) | metadata track (type 33) |

The samples of fields and of RF are therefore carried in **audio tracks** (a field is a block of 16-bit samples). This was tested: ffmpeg writes 16-bit PCM into Matroska and reads it back bit for bit, while it refuses raw grey video there (`gray16le` and `gray10le` are not supported natively, and its VFW mode turns them into another pixel format and loses the data). The kind of the track (`tbc`, `cvbs`, `rf`...) says what the samples are, not the type of the Matroska track, and the sample rate of an audio track is not the real one (next point).

- **The real sample rate is in the parameters of the track.** FLAC declares a rate of at most 655,350 Hz (ffmpeg refuses more), and the sample rate of an audio track is not meant to be the 14 to 40 MHz of a capture. The Matroska or FLAC rate is a stand-in (as `ld-compress` writes 40 kHz for RF), and a reader takes the rate from the parameters.
- **Raw is valid for every stream, and every reader must read it.** In a pipe tools write raw (nothing to spend CPU on).
- **Compression is a choice of the writer.** A file written to be kept is compressed by the rule above. A tool that only converts (a recompression step, in the manner of `ld-compress`) can turn a raw file into a compressed one without touching the rest.
- Decoding gives back **the same samples** that were given to the encoder. A reader returns decoded samples to the tool, so the tool does not know the codec.
- A reader that does not have a codec **refuses the stream with a message that names the codec**. It never reads data it cannot interpret.

Measured on 40 fields of an NTSC capture (910 x 263, size as a share of the raw 16-bit data): 16 bits, FLAC 69 to 87 %, FFV1 75 to 98 %. Values reduced to 10 bits (a simulation, not lossless for a TBC), FFV1 31 to 40 %, FLAC 31 to 34 %. A real `.cvbs` has not been measured yet.

## 5. Time

All blocks have a timestamp. The timeline is the one of the first field of the first frame, in nanoseconds. Tracks of unequal unit (fields, audio blocks) are aligned by their timestamps and not by a count.

## 6. Metadata and annotations

Metadata are blocks of JSON (UTF-8) on a track of kind `metadata` or `annotations`. A track declares a **namespace** in its parameters:

- `ldx.*` is reserved for names defined in this specification (for example `ldx.vbi`, `ldx.vits`, `ldx.dropouts`, `ldx.blacklevel`).
- Anything else is a name chosen by a tool, written in reverse domain order (`com.example.mytool`), so that two tools never write two incompatible `blacklevel`.

Each block says what it applies to (a time, or a range of time) and gives its values. A tool is free to add a track of its own at any time it writes the file. A tool must not rewrite a block it did not write.

When two blocks of the same track, target and key give a value, **the last block written wins**. The earlier block stays in the file. A block can carry a `deleted` flag to cancel an earlier one.

Annotations are the same, with an author, a time of writing, and free content.

## 7. History

The tag `LDX_HISTORY` holds a JSON list, to which each tool that writes the file adds an entry: the program, its version, its options, the time, and the file or files it read. A tool never removes an entry of another one.

## 8. Streams that pass through tools

A tool that reads a file or a pipe and writes another **copies the tracks and blocks it does not understand, unchanged**, and adds its own. Without this rule the first tool that does not know a track would lose it.

## 9. Pipes, and writing after the fact

- A segment and its clusters are written with an unknown size when the output is a pipe. A reader does not need the end of the file.
- Elements that need the end (the index of positions, a final list of tags) are written only if the output can be sought.
- To add data to a finished file (an annotation, a new metadata track), a tool writes new blocks at the end and a new tag list and index, with the space of the old ones turned to padding. This works for a modest amount of data. Adding many values about **old** timestamps may need the file to be written again: this is the weak point of the choice of Matroska, and it must be measured.

## 10. Embedded `.stack`

A stacking session can be an attachment named `session.stack`. Its sources, instead of paths, are the numbers of tracks of the same file, so that one file holds both the data and the way they are stacked. A `.stack` file on its own keeps working unchanged.

## 11. Reading rules

- A track kind or a codec that is unknown: the stream is skipped (kind) or refused with the name (codec).
- A field of the parameters that is unknown is ignored.
- A change of the profile version that makes an older reader wrong is a change of its major number, and a reader refuses a major number it does not know.

## 12. Left to the first implementation

These do not change what a file contains. The prototype settles them, and the result is written here.

1. **Late annotations** about old timestamps: how much data is acceptable before the file is written again. Measure.
2. A real **`.cvbs`** to measure FFV1 against FLAC (the figures in section 4 come from a simulation at 10 bits).
3. **A TBC reduced to 10 bits** (lossy): allowed or not, and how it is recorded in the history. Not allowed by this version: a `tbc` track is 16 bits.
4. **Frame as unit** rather than field for the metadata of a frame (the time stamp of the first field of the frame is used meanwhile).
5. What **VLC** and ffmpeg show for the tracks.

## 13. Decisions of this version

- Name and extension: LDX, `.ldx`.
- A Matroska profile, with the tag `LDX_PROFILE` = 1 and the DocType `matroska`.
- Codecs by depth: 16 bits raw or FLAC, 10 bits raw or FFV1, raw everywhere in a pipe.
- Raw samples in PCM audio tracks, the real rate in the parameters.
- Namespaced metadata, the last block wins, history in `LDX_HISTORY`, unknown blocks copied.
- Compatibility with ld-decode and decode-orc is not a goal of 1.0.
- Libraries: **libebml and libmatroska** (LGPL-2.1-or-later, in vcpkg, compatible with the GPL-3 of the tools) read and write the container, which gives control of every element: custom tracks, tags, attachments, padding, unknown sizes for pipes, copying unknown blocks. **ffmpeg (libavcodec)** is an optional back end for FLAC and FFV1 only. Raw data, metadata and the container need no ffmpeg, so a reader without it still reads every raw file, as section 4 requires. A library that writes Matroska but does not keep unknown elements (libavformat) is not used for the container.

## 14. Next steps

1. A prototype, `ld-container pack | unpack | info`, with a `tbc` or `cvbs` stream, a `pcm` stream and a `metadata` track.
2. Measure what section 12 lists, and write the results here.
3. Use the format in the tools: the library reads `.ldx` as it reads `.tbc`, `.cvbs` and `.stack`.
