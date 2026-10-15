# Working with LDX files

An **LDX file** (`capture.ldx`) keeps everything about one capture, or about several, in a single file: the video (TBC or CVBS), its metadata, the analogue audio, the EFM and AC3 data, and a history of what was done to it. It replaces the set of files a capture usually is (`capture.tbc`, `capture.tbc.db`, `capture.pcm`, `capture.efm`...) when it is shared, archived or sent from one tool to another.

An LDX file is a Matroska file (so ffmpeg and VLC can open it) with a few conventions of its own. The full description is in the [specification](../Specifications/container-format.md); this page is about using it.

## Why use one

- **One file to move around.** The metadata cannot get separated from the video, and the audio is where it belongs.
- **Smaller files, with nothing lost.** The 16-bit TBC and the audio can be kept as FLAC (about 70 % of their size), the 10-bit CVBS as FFV1 (about 35 %). What the tools read is exactly what was written.
- **Pipes.** An LDX stream can go from one tool to the next without a file in between (`ld-container pack capture.tbc -o - | ld-analyse -`). The tool that reads it is told by the stream what the video is.
- **Several captures in one file.** The sources of a stack, or several captures of the same VHS tape, are tracks of one file, added or removed by their id. A stacking session (`.stack`) can be kept in the file and refer to those tracks, so it does not depend on the path of any file.
- **A history.** Each tool that changes a capture records what it did, when, and with which arguments.

## Making and unpacking a file

```
ld-container pack capture.tbc --pcm capture.pcm --efm capture.efm -o capture.ldx
ld-container pack capture.tbc --codec auto -o capture.ldx      # FLAC for the TBC and the audio
ld-container pack capture.cvbs --codec auto -o capture.ldx     # FFV1 for CVBS
ld-container info capture.ldx                                  # what is in it
ld-container verify capture.ldx                                # every block reads back
ld-container unpack capture.ldx -o capture-files               # the separate files again
```

`unpack` gives back the files the capture was made of, byte for byte. `ld-container recompress` changes the codec of a file that exists, `add` and `remove` put a capture in or take one out of a file, and `stack --embed` keeps a stacking session in it. See [ld-container](../Tools/ld-container.md).

## Using an LDX file with the tools

Where a tool takes a `.tbc` (or `.cvbs`) file, it takes an `.ldx` file:

```
ld-analyse capture.ldx
ld-chroma-decoder capture.ldx out.rgb
ld-process-vbi capture.ldx            # the new metadata are added to the file
ld-disc-stacker capture.ldx other.ldx stacked.ldx
```

A file with several captures is opened by the number of the track: `capture.ldx#3`. Without a number the tools take the capture of the stacking session the file keeps, or else its first video track.

The tools that write video (`ld-dropout-correct`, `ld-discmap`, `ld-disc-stacker`) can write an `.ldx` file as their output. An LDX file holds TBC or CVBS video as it is and the tools read both, so there is nothing to convert (`ld-tbc-to-cvbs` and `ld-cvbs-to-tbc` are for the separate files). The metadata export tools (`ld-export-metadata`, `ld-export-decode-metadata`) read an LDX file like a metadata file. `ld-process-efm` and `efm-decoder-f2` read the EFM track of an LDX file (or, for `efm-decoder-f2`, of a stream). The AC3 tools take RF and make AC3 data, so they have no LDX input: `ld-container extract` gives the RF or the AC3 of a file.

## Pipes

`-` is the standard input or output:

```
ld-container pack capture.tbc -o - | ld-process-vbi - | ld-process-vits - | ld-analyse -
ld-container pack capture.cvbs --codec ffv1 -o - | ld-chroma-decoder - out.rgb
```

Each tool in the middle (`ld-process-vbi`, `ld-process-vits`) adds the metadata of every field to the stream as the field goes by, so the next tool does not wait for the end. `ld-disc-stacker`, `ld-dropout-correct` and `ld-discmap` can also end a chain (`... | ld-dropout-correct - - | ld-analyse -`): their output on `-` is a stream with the metadata of each field. `ld-disc-stacker` and `ld-dropout-correct` need the metadata of the whole capture before they begin, and refuse a stream that does not send it. A stream is read once and in order: `ld-container verify` needs a file. See the section on pipes in the specification.

## The history

```
ld-container history capture.ldx          # time, tool, version, what it did, arguments
ld-container history capture.ldx --json   # the same, for a program
```

## Building

The LDX support needs libebml and libmatroska, and ffmpeg's libavcodec for FLAC and FFV1. Without ffmpeg a file is read and written raw; without libebml and libmatroska the LDX tools are not built. The Nix shell and `build.bat` supply all three; see [BUILD.md](../../BUILD.md).
