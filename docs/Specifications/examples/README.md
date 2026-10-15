# LDX examples and test vectors

Small files that cover the format (see the [specification](../container-format.md)), and what each must give. They are made by `make-examples.ps1` with `ld-container` from made-up captures (ramps of samples). A file is not byte-for-byte reproducible (the tracks have random UIDs and the history has the time), so the checks are on **what a reader returns**: the units of each track, whose SHA-256 sums are below, and the current metadata.

A reader passes if, for each file, it lists the tracks of the table, and returns for each track the bytes (the units end to end) with the SHA-256 given. A writer passes if `ld-container verify` says the file is sound and `ld-container unpack` gives back the SHA-256 sums below.

```
ld-container info tbc-raw.ldx
ld-container verify tbc-raw.ldx
ld-container unpack tbc-raw.ldx -o out
```

## The inputs

| File | Bytes | SHA-256 |
|---|---|---|
| `tiny.tbc` (4 fields of 16 x 8 samples, NTSC) | 1,024 | `191919f892445d7aea0c5598f6b998cbb3a611cd5af0f4feb2c383800cf29d8f` |
| `tiny_chroma.tbc` | 1,024 | `251d7b04048c25f51eb7780e2222c8b4ac17ffdde6cd35ebe3cf2e52b978f917` |
| `tiny.pcm` (100 stereo samples, 16-bit, 44,100 Hz) | 400 | `2e1345f1fc63dacca0c177ceaba46d651c41ce3368809f62324010c69d178464` |
| `tiny.efm` | 300 | `95f267b5cfda93763d87c0121cefc7dcdee75ce2d51aaf65e4386fe61eededa4` |
| `tiny.cvbs` (2 NTSC frames of 477,750 samples) | 1,911,000 | `e46382402b74d61bf20b9985f825fd82ce62085be587cf3e6c13bc0421f41011` |
| `tiny_audio_0.wav` (100 stereo samples, 24-bit, 48,000 Hz) | 644 | `c422f4b9314a316b74193e02dee667e24c548ebe906d48bb3cdd25849f0d6f31` |

## The files

### `tbc-raw.ldx`: everything raw

Tracks: `1 tbc raw 16` (4 blocks, the times 0, 16.683, 33.367, 50.050 ms), `2 metadata raw` (1 block, a whole set, about track 1), `3 pcm raw 16` (1 block, 44,100 Hz, about track 1), `4 efm raw 8` (1 block, about track 1).

| Track | Returns | SHA-256 |
|---|---|---|
| 1 | the 4 fields end to end (= `tiny.tbc`) | `191919f8...29d8f` (as `tiny.tbc`) |
| 2 | the current metadata, 4 fields, 852 bytes as `unpack` writes them | `a3976cbfce48d0f7e2050efeeeb605204e6e0ed3727c835387c5f18f1a3412b5` |
| 3 | = `tiny.pcm` | `2e1345f1...78464` |
| 4 | = `tiny.efm` | `95f267b5...eded a4` |

### `tbc-flac.ldx`: the same capture with FLAC

Tracks 1 and 3 have `"codec": "flac"` (`A_FLAC`, laced blocks, the top bit of the TBC samples flipped before coding), track 2 is the same as above. A reader returns **the same bytes as for `tbc-raw.ldx`** (all sums as above, no efm track).

### `tbc-stream.ldx`: metadata that came field by field

The metadata track has **5 blocks**: the parameters alone (no fields, `numberOfSequentialFields` 0) and then one block for each of the 4 fields, each stamped with the time of its field and **before** the video block of that field. The current metadata (section 8.3) is **the same as for `tbc-raw.ldx`**: `a3976cbf...412b5`. The file is a finished one (with cues); a stream written to a pipe has the same blocks and no cues.

### `tbc-nocues.ldx`: a stream saved as it came

The same blocks as `tbc-stream.ldx`, written as a producer writes them to a pipe and saved to a file: **no cues**, and a segment whose size is not known. A reader opens it as a file (section 16), makes the index by reading it, and returns the same bytes for every track as `tbc-stream.ldx` (the sums above, and `a3976cbf...412b5` for the metadata).

### `tbc-yc.ldx`: a Y/C capture

Tracks: `1 tbc` (luma, = `tiny.tbc`), `2 metadata` (about track 1), `3 tbc` with `"role": "chroma"` and `"track": 1` (= `tiny_chroma.tbc`, `251d7b04...f917`). The captures of the file are the track 1 alone.

### `cvbs-ffv1.ldx`: CVBS as FFV1, with 24-bit audio

Tracks: `1 cvbs ffv1 10` (2 blocks = frames, `lineWidth` 910, `frameSamples` 477,750, `sampleEncoding` `CVBS_U10_4FSC`; an image of 910 x 525, 10-bit grey), `2 metadata` (about track 1), `3 pcm raw 24` (1 block, 100 stereo samples, 48,000 Hz, `"bits": 24`).

`unpack` gives `tiny.cvbs` back byte for byte (`e4638240...41011`) and the WAV (`c422f4b9...f6f31`). A reader that returns the units as the 10-bit values in words (section 5.2) gets, for the encoding `CVBS_U10_4FSC`, the words of the `.cvbs` file.
