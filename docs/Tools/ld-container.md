# ld-container

`ld-container` packs captures into an LDX file (see [the specification](../Specifications/container-format.md)), adds and removes captures, keeps a stacking session in the file, shows what the file holds, and writes its tracks out as separate files again.

This is a first slice: the tracks are raw (no FLAC or FFV1 yet), and the tool reads a `.tbc` with its metadata and an optional 16-bit stereo PCM file.

## Usage

```
ld-container pack input.tbc -o file.ldx [--pcm file.pcm] [--efm file.efm] [--ac3 file.ac3] [--pcm-rate 44100] [--start field] [--fields count] [--name name]
ld-container add file.ldx input.tbc -o new.ldx [same options]
ld-container remove file.ldx --id 3 -o new.ldx
ld-container stack file.ldx session.stack -o new.ldx [--embed]
ld-container info file.ldx
ld-container verify file.ldx
ld-container extract file.ldx --id 4 [-o file]
ld-container unpack file.ldx -o directory
```

- `pack` writes a `tbc` track (one block per field), a `metadata` track holding the ld-decode metadata as JSON, and a `pcm` track if `--pcm` is given. `--start` and `--fields` pack a part of the capture. The metadata of a part are those of its fields, numbered again from 1. The history of the file records the packing.
- `add` gives a capture a track of its own (with its own metadata track and audio), for the sources of a stack or for several captures of the same VHS tape. The file is written again, to the output, with the new track at the next free id.
- `remove` takes out the track with an id, and the metadata and audio that are about it. The id is not given to another track later. If the file keeps a session, the sources of the session that are the removed track are taken out of it, and the tool says so.
- `stack` keeps a stacking session in the file (as the attachment `session.stack`). A source of the session is a **track of the file, named by its id** (`"track": 3`), a **link** to a file or to a track of another LDX file (`"file": "other.ldx", "track": 2`), or, with `--embed`, a `.tbc` that the tool **brings into the file** as a track (with its metadata, and its audio if a `.pcm` of the same name is beside it) and then names by its id. Without `--embed` the files stay links, written relative to where the LDX file is, so that nothing is copied. Every track the session names in the file, or in another LDX file that is there, must exist.
- `info` lists the tracks (id, kind, codec, depth, name, parameters, the number and size of their blocks), the attachments (and the tracks a session uses), the next free id, and the history.
- `extract` writes the data of one track as it is, to a file or, without `-o`, to the standard output, to be piped (`ld-container extract capture.ldx --id 4 | ld-process-efm ...`). For a metadata track it writes the current block, the last one.
- `verify` reads every block of the file in the order it was written and by its place in its track (the way the tools read a capture), and says if the two disagree.
- `unpack` writes one file per track (`name.<id>.tbc`, `name.<id>.pcm`, the metadata as `name.<id>.tbc.json`) and the attachments (`session.stack`).
- `pack --efm` and `--ac3` add the EFM and AC3 files as tracks (cut in chunks, stamped over the time of the video, put end to end they are the file).

An LDX file is a Matroska file: `ffprobe` lists its tracks, and `ffmpeg -i file.ldx -map 0:0 -f s16le out.bin` writes the samples of the first track as raw 16-bit data, identical to the fields that were packed.

## Limits of this version

- Only raw tracks are written; FLAC and FFV1 come with the codec back end.
- The time stamps have a resolution of one microsecond.
- `add`, `remove` and `stack` write the file again, to another file.
- ld-analyse, ld-chroma-decoder, ld-disc-stacker, ld-dropout-correct, ld-discmap, ld-export-metadata, ld-tbc-to-cvbs and ld-stacker-gui open an LDX file, or a track of it (`file.ldx#3`), and the session it keeps; ld-process-vbi and ld-process-vits save their metadata into it, as a block added at the end. The audio is not read by the graphical programs yet, a `cvbs` track, the FLAC and FFV1 codecs and the pipe are still to come, and the EFM tools read the track through `extract`.
