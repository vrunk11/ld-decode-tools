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

Each stream is a **Matroska track**. A track has a *kind*, a *bit depth* (the useful bits of a sample), a *codec*, and parameters. They are written as a JSON object (UTF-8) in a **tag of the track** (`LDX_TRACK`, whose target is the track), so that a tool that does not know the track can still list it. (A raw track also has it in its `CodecPrivate`, as the first files did; the `CodecPrivate` of a FLAC track is the data its decoder needs, so the tag is the way that works for all.)

| Kind | Content | Unit of a block | Depth |
|---|---|---|---|
| `tbc` | the fields of a TBC | one field | 16 |
| `cvbs` | the frames of a CVBS capture, their samples as 10-bit values (section 18) | one frame | 10 |
| `rf` | RF samples | a fixed number of samples | 16 |
| `pcm` | the analogue audio of the disc | a fixed number of samples | 16 |
| `efm` | EFM t-values | one frame (variable size) | 8 |
| `ac3` | AC3 data | one frame (variable size) | 8 |
| `metadata` | JSON, see section 6 | any | text |
| `annotations` | free text or JSON written by a person or a tool, see section 6 | any | text |

A block carries the data of one unit and is stamped with the time of the unit (the field rate for fields, in nanoseconds). Fields of a frame follow each other, first field first. A kind that is not in this table is ignored by a reader that does not know it, and must be copied on (section 8).

**Several streams of the same kind.** A file can hold any number of tracks of one kind, for example the captures of the same VHS tape or the sources of a stack, each a `tbc` or `cvbs` track with its own parameters. A track is identified by its **id**, which is its Matroska track number: it is never reused in a file, and everything that refers to a stream (a `.stack` session, a metadata block, an annotation) refers to it by that id. A track has a `name` for people (for example `capture 1`).

**Adding and removing a stream.** A tool can add a stream to a file, or remove the stream with the id *x*:

- *Adding* gives the new track the next free id. The first implementation writes the file again with the new track (the blocks of the other tracks are copied and merged by time with the new ones). Free space (padding) is kept after the list of tracks so that a later version can add a track without moving the data.
- *Removing* writes the file again without the track and its blocks. The id of a removed track is never given to another track: the tag `LDX_NEXT_TRACK_ID` holds the next id to give, and it only goes up.
- A metadata or annotation track says which track it is about in its parameters (`"track": id`), so that each capture of a file has its own ld-decode metadata. Removing a track removes the metadata tracks that are about it.
- A `.stack` session, a metadata block or an annotation that refers to a removed id is not valid, and the tool that removes the stream must say so (or remove them with it).

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

All blocks have a timestamp. The resolution of a timestamp in the file is **one microsecond** (the Matroska timestamp scale is 1000 ns), so a time given in nanoseconds is rounded to a microsecond. The timeline is the one of the first field of the first frame, in nanoseconds. Tracks of unequal unit (fields, audio blocks) are aligned by their timestamps and not by a count.

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

A stacking session can be an attachment named `session.stack` (media type `application/json`), in the format of the `.stack` files. Each of its sources names **a track of the same file by its id** (`"track": 2`) instead of a path, so that the session does not depend on where any file is on a disk, and the file can be moved, shared or renamed. One file then holds the captures (the sources of a stack, or several captures of the same VHS tape) and the way they are stacked.

A source of a session kept in a file names its capture in one of three ways, so that a session can embed the captures it uses, link to them, or mix the two (a file that copies every source can be heavy, and a session can point at another LDX file instead):

| Source | Meaning |
|---|---|
| `"track": 2` | a track of the same LDX file |
| `"file": "other.ldx", "track": 2` | a track of another LDX file (a link) |
| `"file": "capture.tbc"` | a file outside this one (a link): a TBC, a CVBS, an LDX file or a `.stack` session |

The path of a link is relative to where the `.ldx` file is, so that the two can be moved together. A tool that writes a session into a file can **embed** the sources that are files (it then copies each into a track of its own, with its metadata and audio, and names it by its id) or **leave them as links**; the choice is the tool's, and both give a valid file. A tool that adds or removes a stream updates the sources that are tracks of the same file: a removed track is taken out of the session, and the tool says so. Links are not touched. A `.stack` file on its own keeps working unchanged, and a session can be taken out of a file and put back.

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
- Libraries: **libebml and libmatroska** (LGPL-2.1-or-later, in vcpkg, compatible with the GPL-3 of the tools) read and write the container, which gives control of every element: custom tracks, tags, attachments, padding, unknown sizes for pipes, copying unknown blocks. **ffmpeg (libavcodec)** does the encoding and decoding of FLAC and FFV1, and nothing else: a block is encoded by ffmpeg, then wrapped by our own code into the container (and unwrapped before decoding). It is optional. Raw data, metadata and the container need no ffmpeg, so a reader without it still reads every raw file, as section 4 requires. A library that writes Matroska but does not keep unknown elements (libavformat) is not used for the container.

## 14. Next steps

1. A prototype, `ld-container pack | unpack | info`, with a `tbc` or `cvbs` stream, a `pcm` stream and a `metadata` track.
2. Measure what section 12 lists, and write the results here.
3. Use the format in the tools: the library reads `.ldx` as it reads `.tbc`, `.cvbs` and `.stack`.

## 15. How the tools name and open a capture

The tools take an LDX file where they take a `.tbc`, and say which capture with the id of its track:

| Name | What the tool reads |
|---|---|
| `capture.ldx` | the stack of the session the file keeps (section 10), or, when it keeps none, its first `tbc` track |
| `capture.ldx#3` | the track 3 (a `tbc` track, with the metadata track that is about it) |

A source of a session that is a track of another file is the same name (`"file": "other.ldx", "track": 3` is `other.ldx#3`). The metadata of a capture are read from its metadata track, and the metadata of a session are those of its first source that is on, as for a `.stack` file. The tools do not write metadata into an LDX file: `ld-container` does.

The first implementation reads `tbc` tracks that are raw or FLAC. A `cvbs` track is read as the fields of its frames (section 18); FFV1 needs ffmpeg.

## 16. Adding to a file in place

A tool that adds blocks to a file that is already written does it where the file ends, and moves nothing: the new clusters go where the cues were, the cues are written again with the blocks that were there and the new ones, the size of the segment is corrected, and the file is cut after the cues. Tags written at the end hold the history, with what was done added; the last tags of the file are the current ones. A file that is cut short while it is added to can be taken back to its last cues.

**The metadata of a capture** (`ldx.tbcmetadata`, the ld-decode metadata as JSON) are one block of a metadata track whose parameters say which track it is about (`"track": 1`). **The last block of that track is the current metadata**: a tool that changes them (ld-process-vbi, ld-process-vits, any tool that now saves metadata) adds a new block, stamped after everything that is in the file, and what was there stays until the file is written again. `ld-container unpack` and `extract` write the last block.

**EFM and AC3** are tracks of kind `efm` and `ac3` (8 bits). The file they come from has no time of its own, so it is cut in chunks of 64 KiB, which are stamped in proportion to where they are in the file over the time of the video; the chunks put end to end are the file. Their parameters say which track they go with.

**Analogue audio** is a `pcm` track (16-bit stereo, the real sample rate in its parameters, blocks of whole samples one after the other). A tool that wants the samples from a number reads them from the track about the capture: the first sample of a block is found from its time and the sample rate.

## 17. FLAC

A track of 16-bit samples (`tbc`, `rf`, `pcm`) can be kept as FLAC (`"codec": "flac"` in the description of the track). It is the Matroska codec `A_FLAC`, so that its `CodecPrivate` is `fLaC` and the STREAMINFO block, and a player that knows Matroska lists the track.

- **A block of the file holds one block of samples** (a field, a piece of audio) as the FLAC frames of the encoder, laced (EBML lacing). Frames are of 32,768 samples, so a block is at most 8 frames, **262,144 samples** per channel (a field of a TBC is 239,330). A longer block is refused.
- **Unsigned samples** (`tbc`) are made signed before they are coded, by taking 32,768 from them (the top bit flipped), and the other way after, so that the wave does not jump where it crosses the middle of the range. A reader of FLAC that does not know this sees the samples with the top bit flipped.
- **Stereo audio** (`pcm`, two channels) is coded as two FLAC channels together, not as one channel of samples that alternate.
- **Sample rate**: the rate in the FLAC and Matroska data is a stand-in (48,000 Hz), as for raw tracks. The real rate is in the parameters of the track.
- **ffmpeg** (libavcodec) encodes and decodes; the container is written by the library. A build without ffmpeg reads and writes raw tracks only, and says so for a FLAC track.
- **Other programs**: `ffprobe` lists a FLAC track, and `ffmpeg` decodes it (`-af asetpts=N/SR/TB` to give the samples a continuous time, since a block holds more samples than its place in the file's time stands for).

Measured on a NTSC capture, the tracks of the TBC and the audio together are about **69 to 71 %** of their raw size.

## 18. CVBS and FFV1

A capture of CVBS (a `.cvbs` file) is a track of kind `cvbs`: **a block is a frame** of the file, whose samples are given as **10-bit values in 16-bit words** (the CVBS_U10 encoding, whatever the encoding of the file the frame comes from; the encoding it had is in the parameters, so that the file can be given back as it was). The parameters of the track are the video system, `lineWidth` (the samples of a line), `frameSamples` (the samples of a frame: 477,750 for NTSC, 709,379 for PAL, 477,225 for PAL-M), `sampleEncoding`, and `padValue` (the blanking level).

- **Raw**: the frame as 16-bit words, like the other tracks of samples.
- **FFV1** (`"codec": "ffv1"`): a video track (`V_FFV1`, level 3, every frame a key frame, 10-bit grey), whose image is as wide as a line and has the rows that fit. **When the last row is not whole it is filled up with `padValue`**: a PAL frame is 625 lines and 4 samples, so its image has 626 rows (the frame is cut off again after it is decoded). NTSC and PAL-M frames fill their images. A player that knows Matroska shows the track as video of 910 x 525 (NTSC) or 1135 x 626 (PAL).
- **What the tools read**: a cvbs track is read as the two fields of the TBC that a frame maps to, the way a `.cvbs` file is, so a capture of CVBS kept in an LDX file stacks, decodes and shows as one kept in a file. The result of a stack of captures of CVBS is CVBS (a `.cvbs` file), as for files.
- **Measured** on a part of the NTSC capture kept as CVBS: the track as FFV1 is **34 %** of its size as raw samples, and `unpack` gives the `.cvbs` file back identical, byte for byte.
- A Y/C pair (`.cvbsy`, `.cvbsc`) and the audio of a CVBS capture (WAV files) are not tracks yet.
