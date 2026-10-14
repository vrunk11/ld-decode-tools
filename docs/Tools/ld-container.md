# ld-container

`ld-container` packs a capture into an LDX file (see [the specification](../Specifications/container-format.md)), shows what an LDX file holds, and writes its tracks out as separate files again.

This is a first slice: the tracks are raw (no FLAC or FFV1 yet), and the tool reads a `.tbc` with its metadata and an optional 16-bit stereo PCM file.

## Usage

```
ld-container pack input.tbc -o output.ldx [--pcm file.pcm] [--pcm-rate 44100] [--start field] [--fields count]
ld-container info file.ldx
ld-container unpack file.ldx -o directory
```

- `pack` writes a `tbc` track (one block per field), a `metadata` track holding the ld-decode metadata as JSON, and a `pcm` track if `--pcm` is given. `--start` and `--fields` pack a part of the capture. The history of the file records the packing.
- `info` lists the tracks (id, kind, codec, depth, name, parameters, the number and size of their blocks) and the history.
- `unpack` writes one file per track: `.tbc`, `.pcm`, and the metadata as `.tbc.json`. Several tracks of the same kind are told apart by their id in the file name.

An LDX file is a Matroska file: `ffprobe` lists its tracks, and `ffmpeg -i file.ldx -map 0:0 -f s16le out.bin` writes the samples of the first track (the `tbc` track) as raw 16-bit data, identical to the fields that were packed.

## Limits of this version

- Only raw tracks are written; FLAC and FFV1 come with the codec back end.
- The time stamps have a resolution of one microsecond.
- The EFM and AC3 tracks, adding or removing a stream of a file, and the library reading `.ldx` directly in the other tools are still to come.
