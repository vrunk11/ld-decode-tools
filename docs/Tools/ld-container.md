# ld-container

`ld-container` packs captures (TBC or CVBS) into an LDX file (see [the specification](../Specifications/container-format.md)), adds and removes captures, keeps a stacking session in the file, shows what the file holds, and writes its tracks out as separate files again.

This is a first slice: the tracks are raw (no FLAC or FFV1 yet), and the tool reads a `.tbc` with its metadata and an optional 16-bit stereo PCM file.

## Usage

```
ld-container pack input.tbc -o file.ldx [--pcm file.pcm] [--codec flac|ffv1|auto] [--live-metadata] [--efm file.efm] [--ac3 file.ac3] [--pcm-rate 44100] [--start field] [--fields count] [--name name]
ld-container add file.ldx input.tbc -o new.ldx [--mapped-from 1] [same options]
ld-container remove file.ldx --id 3 -o new.ldx
ld-container stack file.ldx session.stack -o new.ldx [--embed]
ld-container info file.ldx
ld-container recompress file.ldx --codec flac|ffv1|auto [--id 1] -o new.ldx
ld-container verify file.ldx
ld-container extract file.ldx --id 4 [-o file]
ld-container unpack file.ldx -o directory
```

- `pack` writes a `tbc` track (one block per field), a `metadata` track holding the ld-decode metadata as JSON, and a `pcm` track if `--pcm` is given. `--start` and `--fields` pack a part of the capture. The metadata of a part are those of its fields, numbered again from 1. The history of the file records the packing.
- `add` gives a capture a track of its own (with its own metadata track and audio), for the sources of a stack or for several captures of the same VHS tape. The file is written again, to the output, with the new track at the next free id. **`--mapped-from <id>`** says that the capture is the mapping (ld-discmap) of the video track with that id, which has to be in the file (and is not a chroma track): the new track gets `"mappedFrom": <id>` in its parameters (section 7.4 of the specification), and the tools then find it from the original. ld-stacker-gui uses it to keep the mapping of a track beside the capture.
- `remove` takes out the track with an id, and the metadata and audio that are about it. The id is not given to another track later. If the file keeps a session, the sources of the session that are the removed track are taken out of it, and the tool says so.
- `stack` keeps a stacking session in the file (as the attachment `session.stack`). A source of the session is a **track of the file, named by its id** (`"track": 3`), a **link** to a file or to a track of another LDX file (`"file": "other.ldx", "track": 2`), or, with `--embed`, a `.tbc` that the tool **brings into the file** as a track (with its metadata, and its audio if a `.pcm` of the same name is beside it) and then names by its id. Without `--embed` the files stay links, written relative to where the LDX file is, so that nothing is copied. Every track the session names in the file, or in another LDX file that is there, must exist.
- `info` lists the tracks (id, kind, codec, depth, name, parameters, the number and size of their blocks), the attachments (and the tracks a session uses), the next free id, and the history.
- `extract` writes the data of one track as it is, to a file or, without `-o`, to the standard output, to be piped (`ld-container extract capture.ldx --id 4 | ld-process-efm ...`). For a metadata track it writes the current block, the last one.
- `verify` reads every block of the file in the order it was written and by its place in its track (the way the tools read a capture), and says if the two disagree.
- `unpack` writes one file per track (`name.<id>.tbc`, `name.<id>.pcm`, the metadata as `name.<id>.tbc.json`) and the attachments (`session.stack`).
- `pack --live-metadata` sends the metadata as a producer fed by a pipe does: the parameters of the video first, then the metadata of each field in a block of its own, just before its video (specification, section 15). The tools that read a stream take the fields from them as they come; it is how the chain `ld-decode | tool | tool | ld-analyse -` can add metadata along the way.
- `pack --efm` and `--ac3` add the EFM and AC3 files as tracks (cut in chunks, stamped over the time of the video, put end to end they are the file).

An LDX file is a Matroska file: `ffprobe` lists its tracks, and `ffmpeg -i file.ldx -map 0:0 -f s16le out.bin` writes the samples of the first track as raw 16-bit data, identical to the fields that were packed.

## Limits of this version

- FLAC (the TBC and the audio) and FFV1 (CVBS) are written and read; the Y/C pairs (TBC and CVBS) and the audio of CVBS captures are tracks.
- The timestamps have a resolution of one microsecond.
- `add`, `remove` and `stack` write the file again, to another file.
- ld-analyse, ld-chroma-decoder, ld-disc-stacker, ld-dropout-correct, ld-discmap, ld-export-metadata, ld-tbc-to-cvbs and ld-stacker-gui open an LDX file, or a track of it (`file.ldx#3`), and the session it keeps; ld-process-vbi and ld-process-vits save their metadata into it, as a block added at the end. The audio is not read by the graphical programs yet, a `cvbs` track, the pipe is still to come, and the EFM tools read the track through `extract`.

## Compression

`--codec flac` (on `pack`, `add` and `stack --embed`) keeps the TBC and the audio as FLAC, which is about 69 to 71 % of their size as raw samples; `--codec ffv1` keeps CVBS as FFV1 (about 34 %), and `--codec auto` does both. `ld-container recompress file.ldx --codec flac -o new.ldx` changes a file that is already written, and `--codec raw` brings it back; `--id 1` changes one track only. The samples that come out are the ones that went in, whatever the codec. `info` shows the size of the file against what it holds. The tools that open an LDX file need ffmpeg in their build to read a FLAC track.

## CVBS

`ld-container pack capture.cvbs -o file.ldx` packs the frames of a composite CVBS file (with its metadata) as a `cvbs` track, and `--codec ffv1` (or `auto`) keeps it as FFV1, which is about a third of the raw size. `add` and `stack --embed` take `.cvbs` files too. `unpack` gives the `.cvbs` file back (as the encoding it had), with its `.meta` and the companion metadata beside it. `--codec auto` is the best codec for each track: FLAC for the TBC and the audio, FFV1 for CVBS; `flac` and `ffv1` are for the tracks they suit and leave the others raw. A frame is not cut in two: `--start` and `--fields` of a CVBS capture are in fields and must be whole frames.

## Piped LDX

`-` as the output of `pack`, `add`, `remove`, `stack` and `recompress` writes the LDX stream to the standard output, and `-` as the file of `info`, `unpack`, `extract`, `recompress`, `add`, `remove` and `stack` reads it from the standard input, so that tools can be chained:

```
ld-container pack capture.tbc --pcm capture.pcm -o - | ld-container recompress - --codec flac -o - | ld-analyse -
ld-container pack capture.cvbs --codec ffv1 -o - | ld-chroma-decoder - out.rgb
```

A stream has no index: what reads a pipe reads it once, in order. A stream that is saved to a file (`ld-container pack capture.tbc -o - > capture.ldx`) is a file like any other: the tools make its index by reading it the first time it is opened (`verify`, `#N`, `ld-analyse`...), and `ld-container recompress capture.ldx -o indexed.ldx` writes it with its index so that this is not needed again. The messages go to the standard error when the standard output carries the data. See the specification (section 15).

`ld-container history file.ldx` shows the history of a file: for each entry the time, the tool and its build, what it did, and the arguments it was given (`--json` for another program). Every tool that changes a capture adds an entry (specification, section 12).

A CVBS capture packed with `ld-container pack capture.cvbs` takes its audio `capture_audio_0.wav` with it, as a 24-bit `pcm` track (raw), and `unpack` writes the WAV back as it was.

A Y/C capture packs with its chroma: `ld-container pack capture.tbc` (or `capture.tbcy`) takes `capture_chroma.tbc` (or `capture.tbcc`) beside it as a second `tbc` track with `"role": "chroma"`, about the luma track. `unpack` writes it as `<name>.<id>.chroma.tbc`, and `ld-analyse` shows the pair as it shows a Y/C pair of files. A chroma file given alone is refused: pack the luma file. A CVBS Y/C pair works the same: `ld-container pack capture.cvbsy` takes `capture.cvbsc` with it as a second `cvbs` track, and `unpack` writes `capture.<id>.cvbsy` and `.cvbsc` with their `.meta`.

## Audio

`ld-container wav capture.ldx [-o capture.wav] [--source analogue|efm] [--id n]` writes the audio of a file as a WAV: the analogue audio (16-bit, or 24-bit for a CVBS capture) or, with `--source efm`, the digital audio decoded from EFM. The script `scripts/pcm2wav capture.ldx` does the same.

`ld-container addtrack capture.ldx --kind pcm|efm|ac3|annotations --input file [--about id] [--name n] [--rate hz] [--source efm]` puts a file in the LDX file as a track of its own, in its place: the audio decoded from EFM (`--kind pcm --source efm`, 16-bit stereo), AC3 data, EFM t-values or annotations. The audio of EFM can also be put there by the tools that decode it (see `ld-process-efm`).

Every command also takes `-v`/`--version` (the version of the tools), `-d`/`--debug` (show debug messages) and `-q`/`--quiet` (suppress info and warning messages), as the other tools do.
