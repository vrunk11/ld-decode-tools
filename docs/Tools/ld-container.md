# ld-container

`ld-container` packs captures (TBC or CVBS) into an LDX file (see [the specification](../Specifications/container-format.md)), adds and removes captures, keeps a stacking session in the file, shows what the file holds, and writes its tracks out as separate files again.

This is a first slice: the tracks are raw (no FLAC or FFV1 yet), and the tool reads a `.tbc` with its metadata and an optional 16-bit stereo PCM file.

## Usage

```
ld-container pack input.tbc -o file.ldx [--pcm file.pcm] [--codec flac|ffv1|auto] [--live-metadata] [--efm file.efm] [--ac3 file.ac3] [--pcm-rate 44100] [--start field] [--fields count] [--name name] [--medium ld|vhs|vhd|cvbs] [--reference text] [--copy n] [--capture-number n] [--face A|B|AB|BA] [--system sys]
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

The tracks of what a processing tool sends on its standard output (`ld-disc-stacker ... -`) are named `-` (`- audio`, `- metadata`); `recompress` writing to a file gives them the name of that file (`stack`, `stack audio`, `stack metadata` for `-o stack.ldx`). When the tracks of samples are in the codec asked for already, it says so and writes the file as it is.

A stream has no index: what reads a pipe reads it once, in order. A stream that is saved to a file (`ld-container pack capture.tbc -o - > capture.ldx`) is a file like any other: the tools make its index by reading it the first time it is opened (`verify`, `#N`, `ld-analyse`...), and `ld-container recompress capture.ldx -o indexed.ldx` writes it with its index so that this is not needed again. The messages go to the standard error when the standard output carries the data. See the specification (section 15).

`ld-container history file.ldx` shows the history of a file: for each entry the time, the tool and its build, what it did, and the arguments it was given (`--json` for another program). Every tool that changes a capture adds an entry (specification, section 12).

A CVBS capture packed with `ld-container pack capture.cvbs` takes its audio `capture_audio_0.wav` with it, as a 24-bit `pcm` track (raw), and `unpack` writes the WAV back as it was.

A Y/C capture packs with its chroma: `ld-container pack capture.tbc` (or `capture.tbcy`) takes `capture_chroma.tbc` (or `capture.tbcc`) beside it as a second `tbc` track with `"role": "chroma"`, about the luma track. `unpack` writes it as `<name>.<id>.chroma.tbc`, and `ld-analyse` shows the pair as it shows a Y/C pair of files. A chroma file given alone is refused: pack the luma file. A CVBS Y/C pair works the same: `ld-container pack capture.cvbsy` takes `capture.cvbsc` with it as a second `cvbs` track, and `unpack` writes `capture.<id>.cvbsy` and `.cvbsc` with their `.meta`.

## Audio

`ld-container wav capture.ldx [-o capture.wav] [--source analogue|efm] [--id n]` writes the audio of a file as a WAV: the analogue audio (16-bit, or 24-bit for a CVBS capture) or, with `--source efm`, the digital audio decoded from EFM. The script `scripts/pcm2wav capture.ldx` does the same.

`ld-container addtrack capture.ldx --kind pcm|efm|ac3|annotations --input file [--about id] [--name n] [--rate hz] [--source efm] [--codec raw|flac]` puts a file in the LDX file as a track of its own, in its place: the audio decoded from EFM (`--kind pcm --source efm`, 16-bit stereo), AC3 data, EFM t-values or annotations. `--codec flac` keeps 16-bit audio in FLAC (other tracks are raw). The audio of EFM can also be put there by the tools that decode it (see `ld-process-efm`).

`ld-container reorder capture.ldx --order 3,1,2 -o out.ldx` writes the file again with its captures in that order (the ids of all the video tracks, not the chroma ones). The tracks are numbered again from 1 in the new order: each capture (its video track, its chroma track, then the metadata, audio, EFM, AC3 and notes about it), then the other tracks. The `"track"` and `"mappedFrom"` parameters and the sources of the `session.stack` of the file follow the new numbers; links to other files are not touched. It is the one command that gives an id to another track (section 17 of the specification).

`ld-container index stream.ldx -o file.ldx` writes a stream that was saved as it came again as a file with its index. `ld-container metadata capture.ldx --id n --export file.json` writes the current metadata of a capture as JSON; `--input file.json|file.db [-o out.ldx]` replaces them with those of an ld-decode file (a block added at the end of the file, which is the current one from then on, section 8.3 of the specification; with `-o` a copy gets it). `ld-container unstack capture.ldx -o out.ldx` writes the file again without its session.

`ld-container display capture.ldx --id n --aspect 4:3|16:9|none [-o out.ldx]` sets the shape the video players (VLC, ffplay, mpv) show a capture in: with 4:3 or 16:9 the display aspect ratio of the whole image is written in the header of the video track, the same as ld-analyse shows the capture in (the width adjustment of its system; the image is not cut), and `isWidescreen` of the metadata is set to match (a block added at the end of the file). `none` (the default of a file) shows the whole image with square pixels, 1:1. ld-analyse reads `isWidescreen` as it always did.

`ld-container display` without `-o`, `ld-container rename capture.ldx --id n --name text` and `ld-container addtrack ... --kind annotations --update` change the file itself, in the room the list of tracks has after it (section 2 of the specification): the shape of a capture, the name of a track, and notes added as a track of annotations stamped after everything in the file. No copy of the file is made, and it is quick; when there is not enough room they say so and the file has to be written again (with `-o`). With `-o`, `display` and `rename` give a copy that has the change.

`ld-container settrack capture.ldx --id n [--name text] [--set key=value ...] [--unset key ...] [-o out.ldx]` changes the name and the parameters of a track: the sample rate of the audio (`--set sampleRate=44100`, which is what a player plays it at), the system and the sample rate of a video, and the like (the value is JSON, or text). Without `-o` the file itself is changed, in the room the list of tracks has, with no copy; FLAC audio whose rate changes is coded again, which needs an output.

`ld-container addchroma capture.ldx --about id --input chroma.tbc|.tbcc|.cvbsc [--codec c] -o out.ldx` adds the chroma file of a Y/C capture to the capture that is in the file as its luma (section 7.3 of the specification): a track with the role chroma, about the luma, with as many units as it has. `pack` and `add` take the chroma file that is beside the luma with it, and `--no-chroma` leaves it out. `remove` of the luma takes its chroma with it, as the specification says.

`ld-container merge file.ldx other.ldx [--id n ...] [--exclude n ...] [--name text] [--codec asis|raw|flac|ffv1|auto] -o out.ldx` puts the captures of another LDX file in this one (the ones named by `--id`, else all), each with the tracks that go with it: its chroma, metadata, audio, EFM, AC3 and notes (`--exclude` leaves a track out). The tracks get ids of their own after the ones of the file, and what names a track (the tracks about a capture, the mapping of a capture) follows. The session and the other attachments of the other file are not taken, and the history of the other file is not kept (an entry says what was taken).

`ld-container cover capture.ldx --set picture [-o out.ldx]` puts a picture in the file as its cover (the attachment `cover.png` or `cover.jpg`, section 9 of the specification): at most 800 pixels high, any shape, at most 2 MB. The picture can be a JPEG, a WebP, a PNG, a GIF or a BMP: a PNG or a JPEG that fits is kept as it is, and any other, or one that is too big, is scaled down keeping its shape and written as a JPEG (a JPEG or a WebP) or a PNG (read and written by ffmpeg, and by the library itself for the PNG; a build without ffmpeg takes only PNG pictures that fit or are PNG); `--remove -o out.ldx` takes it out, and `--export picture` writes it out. Without `-o` the cover is put in the file itself, as an attachment written at its end (the last of a name counts, section 9): it is quick, nothing is written again, and the old picture stays in the file until it is written again; with `-o` the output is a copy that has it. The tools show it: ldx-packer, ld-stacker-gui (above the information of a source) and ld-analyse (File > Source information). The picture can also be kept outside the file: `--source ADDRESS` writes `cover.url` (a web address, http or https, or a path of a file, absolute or relative to the folder of the .ldx file, with `/` separators) and `--remove-source` takes it out; the picture inside the file wins when both exist, and no tool fetches a web address by itself. `--link ADDRESS` writes `link.url` (http or https only), which the tools open in the browser when the cover is clicked, and `--remove-link` takes it out. These options go with `--set` and `--remove` in the same call, and work in the file itself (no `-o`) or on a copy (`-o`).

`ld-container stream capture.ldx -o stream.ldx` writes the file as an LDX stream (section 15 of the specification): the segment has no size, there are no cues and nothing is written again, with the tracks, their codecs, the metadata and the attachments kept. With `-o -` the stream goes to the standard output. A program that opens the result makes its index by reading it once; `ld-container recompress` writes it with an index again.

Every command also takes `-v`/`--version` (the version of the tools), `-d`/`--debug` (show debug messages) and `-q`/`--quiet` (suppress info and warning messages), as the other tools do.

`ld-container tag capture.ldx [--id n] [--medium ld|vhs|vhd|cvbs] [--reference text] [--copy n] [--capture-number n] [--face A|B|AB|BA] [--system PAL|NTSC|PAL-M|SECAM|NTSC-VHD] [-o out.ldx]` says what a capture is, as a person knows it (sections 3.1 and 8.1 of the specification): what was captured, a reference of the disc, the number of the physical copy and of the capture, the face, and the system. They are the words the `.rfx` archives of RF-tools use, so that what an archive says is copied as it is. They are parameters of the video track (changed in the file itself, in the room the list of tracks has). **The system** is the one of the track, and `videoParameters.system` of the metadata of the capture is changed with it (a block added at the end of the file), so that the two say the same: `SECAM` has the 625 lines of PAL and `NTSC-VHD` the 525 lines of NTSC, and the tools read the lines from them (see the note on compatibility in section 8.1 of the specification). `settrack --set system=...` does the same. An empty value (`--face=`), or 0 for a number, takes the value away. They are shown by ld-analyse and ld-stacker-gui (the Capture section of the information) and changed by ldx-packer. `pack` and `add` take the same options, so that a capture is described as soon as it is made (`--system` must have the lines of the capture: SECAM for a 625-line one, NTSC-VHD for a 525-line one); a value that the metadata of the file already say is kept when no option gives it.

`ld-container rfsource capture.ldx [--id n] [--set file.json | --export file | --remove] [-o out.ldx]` shows, puts in or takes out where a capture comes from: what is known of the RF file it was decoded from (its name, size, SHA-256, the sample rate and the bits of the RF, its date, the files that sat beside it such as the `.yaml` of the Domesday Duplicator), kept in a track of metadata with the namespace `ldx.rfsource` (section 8.4) as the entry of the capture in an `.rfx` archive spells it, so that a decoder copies it without renaming anything. It is information to trace the capture and has no other use. It is put in at the end of the file (no copy of the file), or in a copy with `-o`; the track follows its capture when a file is written again, merged or reordered, and `unpack` writes it as `name.<id>.rfsource.json`.
