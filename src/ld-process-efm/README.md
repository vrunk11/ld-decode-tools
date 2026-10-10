# ld-process-efm

**Single-step EFM decoder**

## Overview

ld-process-efm decodes the raw digital `.efm` output of ld-decode into digital audio (16-bit stereo PCM) or data, in one step, with audio and data error detection and correction.

The [efm-decoder](../efm-decoder/README.md) suite (`efm-decoder-f2`, `-d24`, `-audio`, `-data`, `efm-stacker-f2`) decodes the same data in separate stages, can stack several captures, and gives finer control. Both are built and installed; use whichever suits the disc.

## Usage

### Basic Syntax
```bash
ld-process-efm [options] <input.efm> <output>
```

## Options

### Common Options
- `-h, --help`: Display help on command-line options
- `-v, --version`: Display version information
- `-d, --debug`: Show debug information
- `-q, --quiet`: Suppress info and warning messages

### Audio Error Handling
- `-c, --conceal`: Conceal corrupt audio data (default)
- `-s, --silence`: Silence corrupt audio data
- `-g, --pass-through`: Pass-through corrupt audio data

### Decoding
- `-p, --pad`: Pad the start of the audio from 00:00 to match the initial disc time
- `-b, --data`: Decode F1 frames as data instead of audio
- `-D, --dts`: Audio is DTS rather than PCM (allow non-standard F3 syncs)
- `-t, --time`: Non-standard audio decode (no time-stamp information)

### Detailed Debug
- `--debug-efmtof3frames`, `--debug-syncf3frames`, `--debug-f3tof2frames`, `--debug-f2tof1frame`, `--debug-f1toaudio`, `--debug-f1todata`: detailed debug output for each decoding stage

## Examples

### Decode digital audio
```bash
ld-process-efm capture.efm capture.pcm
```
The output is raw 44.1 kHz 16-bit stereo PCM; `pcm2wav capture.pcm` (in `scripts/`) wraps it in a WAV file.

### Decode a data disc
```bash
ld-process-efm --data capture.efm capture.bin
```

## See Also

- [docs/Tools/ld-process-efm.md](../../docs/Tools/ld-process-efm.md) - formats, AC-3 and DTS discs, and discs without audio time stamps
- [efm-decoder](../efm-decoder/README.md) - the staged EFM decoder suite
