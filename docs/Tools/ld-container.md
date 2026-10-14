# ld-container

`ld-container` packs captures into an LDX file (see [the specification](../Specifications/container-format.md)), adds and removes captures, keeps a stacking session in the file, shows what the file holds, and writes its tracks out as separate files again.

This is a first slice: the tracks are raw (no FLAC or FFV1 yet), and the tool reads a `.tbc` with its metadata and an optional 16-bit stereo PCM file.

## Usage

```
ld-container pack input.tbc -o file.ldx [--pcm file.pcm] [--pcm-rate 44100] [--start field] [--fields count] [--name name]
ld-container add file.ldx input.tbc -o new.ldx [same options]
ld-container remove file.ldx --id 3 -o new.ldx
ld-container stack file.ldx session.stack -o new.ldx
ld-container info file.ldx
ld-container unpack file.ldx -o directory
```

- `pack` writes a `tbc` track (one block per field), a `metadata` track holding the ld-decode metadata as JSON, and a `pcm` track if `--pcm` is given. `--start` and `--fields` pack a part of the capture. The history of the file records the packing.
- `add` gives a capture a track of its own (with its own metadata track and audio), for the sources of a stack or for several captures of the same VHS tape. The file is written again, to the output, with the new track at the next free id.
- `remove` takes out the track with an id, and the metadata and audio that are about it. The id is not given to another track later. If the file keeps a session, the sources of the session that are the removed track are taken out of it, and the tool says so.
- `stack` keeps a stacking session in the file (as the attachment `session.stack`). Its sources are **tracks of the file, named by their id** (`"track": 3`) instead of paths, so that the file does not depend on where any capture is on a disk. Every track the session names must be in the file.
- `info` lists the tracks (id, kind, codec, depth, name, parameters, the number and size of their blocks), the attachments (and the tracks a session uses), the next free id, and the history.
- `unpack` writes one file per track (`name.<id>.tbc`, `name.<id>.pcm`, the metadata as `name.<id>.tbc.json`) and the attachments (`session.stack`).

An LDX file is a Matroska file: `ffprobe` lists its tracks, and `ffmpeg -i file.ldx -map 0:0 -f s16le out.bin` writes the samples of the first track as raw 16-bit data, identical to the fields that were packed.

## Limits of this version

- Only raw tracks are written; FLAC and FFV1 come with the codec back end.
- The time stamps have a resolution of one microsecond.
- `add`, `remove` and `stack` write the file again, to another file.
- A `.stack` session with `"track"` sources is read and written by the session library, but ld-analyse, ld-chroma-decoder and ld-disc-stacker do not yet open the tracks of an LDX file. The EFM and AC3 tracks and the pipe are still to come.
