# ld-chroma-decoder

**Color Decoder for Time Base Corrected LaserDisc Video**

## Overview

ld-chroma-decoder decodes color (chroma) information from Time Base Corrected (TBC) LaserDisc video files, converting them into standard RGB video suitable for playback and editing. It supports multiple decoding algorithms optimized for PAL and NTSC video systems.

## Usage

### Basic Syntax
```bash
ld-chroma-decoder [options] <input.tbc> <output.rgb>
```

## Options

#### Common Options
- `-h, --help`: Display help on command-line options
- `-v, --version`: Display version information
- `-d, --debug`: Show debug information
- `-q, --quiet`: Suppress info and warning messages

#### Input/Output
- `--input-metadata <filename>`: Specify the input metadata file (default input.db, or input.json if only that exists)
- `--meta <db|json>`: Metadata format to use when both input.db and input.json exist (default db, with a warning)
- `-s, --start <number>`: Specify the start frame number
- `-l, --length <number>`: Specify the length (number of frames to process)
- `-t, --threads <number>`: Specify the number of concurrent threads (default number of logical CPUs)

#### Piped input
Input can be piped: give `-` as the input (or the path of a named pipe). With `--input-metadata` the metadata is
used as for a file. Without it the video system must be said, and everything else has a default, so that a
producer's output can be decoded with no files at all; the video then runs until the input ends.
- `--input-format <tbc|cvbs>`: What the pipe carries: TBC fields (the default), or CVBS frames as decode-orc writes them (`.cvbs`, no `.meta`)
- `--system <pal|ntsc|pal-m>`: The video system (required without metadata)
- `--black-level <number>`, `--white-level <number>`: The `black16bIre` and `white16bIre` of a TBC capture (default: the standard levels of the system; copy them from the capture's metadata for an exact result)
- `--subcarrier-locked <yes|no>`: Whether PAL was sampled locked to the subcarrier, which puts the second field 2 samples off (default yes for CVBS, no for TBC)
- `--sample-encoding <u10|u16|tpg21|s16>`: The sample encoding of a CVBS pipe (default `u10`, `CVBS_U10_4FSC`)
- `--buffer-frames <number>`: Frames read ahead of the one being decoded, and kept behind it (default 32)

The NTSC phase of each field (its place in the 4-field colour sequence) is measured from the colour burst when the metadata has none. A field whose burst cannot be measured gets the phase the sequence expects there, from the nearest measured field before it, or after it (up to 8 frames ahead at the start of the input); a measured phase is never replaced, since one that breaks the sequence is most likely a real break. PAL finds its own phase from the burst of every line.

#### Decoder Selection
- `-f, --decoder <decoder>`: Decoder to use (pal2d, transform2d, transform3d, ntsc1d, ntsc2d, ntsc3d, ntsc3dnoadapt, hvd2d, hvd3d, mono; default automatic)

#### Output Control
- `-p, --output-format <format>`: Output format (rgb, yuv, y4m; default rgb); RGB48, YUV444P16, GRAY16 pixel formats are supported
- `-r, --reverse`: Reverse the field order to second/first (default first/second)
- `-b, --blackandwhite`: Output in black and white
- `--pad, --output-padding <number>`: Pad the output frame to a multiple of this many pixels on both axes (1 means no padding, maximum is 32)

#### Color Adjustment
- `--chroma-gain <number>`: Gain factor applied to chroma components (default 1.0)
- `--chroma-phase <number>`: Phase rotation applied to chroma components (degrees; default 0.0)

#### Video Area Control
- `--ffll, --first-active-field-line, --first_active_field_line <number>`: The first visible line of a field. Range 1-259 for NTSC (default: 20), 2-308 for PAL (default: 22)
- `--lfll, --last-active-field-line, --last_active_field_line <number>`: The last visible line of a field. Range 1-259 for NTSC (default: 259), 2-308 for PAL (default: 308)
- `--ffrl, --first-active-frame-line, --first_active_frame_line <number>`: The first visible line of a frame. Range 1-525 for NTSC (default: 40), 1-620 for PAL (default: 44)
- `--lfrl, --last-active-frame-line, --last_active_frame_line <number>`: The last visible line of a frame. Range 1-525 for NTSC (default: 525), 1-620 for PAL (default: 620)

#### NTSC-Specific Options
- `-o, --oftest`: NTSC: Overlay the adaptive filter map (only used for testing)
- `--chroma-nr <number>`: NTSC: Chroma noise reduction level in IRE (default 0.0)
- `--luma-nr <number>`: Luma noise reduction level in IRE (default 0.0)
- `--ntsc-phase-comp`: NTSC: Adjust phase per-line using burst phase

#### NTSC-Specific 3D Options for Adaptive Threshold:
- `-f ntsc3d`:  Default behavior (threshold = 1.0)
- `-f ntsc3d --chroma-weight 1.0`: Chroma weight for the 3D adaptive filter (default 1.0, higher = more 2D)
- `-f ntsc3d --adapt-threshold 1.5`: More aggressive 3D filtering (less fallback to 2D/1D)
- `-f ntsc3d --adapt-threshold 0.1`: More conservative (more likely to use 2D/1D on motion)

#### Transform Decoder Options
- `--simple-pal`: Transform: Use 1D UV filter (default 2D)
- `--transform-threshold <number>`: Transform: Uniform similarity threshold (default 0.4)
- `--transform-thresholds <file>`: Transform: File containing per-bin similarity thresholds
- `--show-ffts`: Transform: Overlay the input and output FFTs
- `--hvd-iterations <number>`: HVD: Conjugate-gradient iterations of the solver (default 2, more = slower and cleaner)
- `--hvd-strength <number>`: hvd3d: Weight of the neighbouring fields (default 0 = measured on the picture)
- `--hvd-overlap <number>`: hvd3d: Frames on each side of a frame that the decoder looks at (default 1)
- `--hvd-no-fast`: HVD: Turn the fast mode off (the solver runs its whole budget: slower, about the same quality)
- `--hvd-no-bidirectional`: hvd3d: Use only the fields before a field, not the ones after (faster, a little worse)
- `--hvd-selective`: hvd3d: 3D only where the picture is ambiguous, 2D elsewhere
- `--hvd-passes <number>`: hvd3d: Passes of the solver over a window of fields (default 2, 1 to 4)
- `--hvd-no-acc`: HVD: No automatic colour control (the saturation is not brought to the nominal burst)
- `--hvd-smoothness <number>`: HVD: Chroma smoothness (default 1.0; higher is smoother chroma, lower is sharper)
- `--hvd-field-order <order>`: HVD: Order of the fields: auto (from the metadata, the default), top or bottom
- `--hvd-symmetry`: HVD: Add the spectral-symmetry initialisation
- `--hvd-subcarrier <hertz>`: HVD: Frequency of the colour subcarrier in Hz, for a source whose subcarrier is not the standard's (the VHD is 2556818.2)
- `--input-metadata <filename>`: Specify the input metadata file (default input.db, or input.json if only that exists)

#### Active Area Selection
- `--ffll, --first-active-field-line, --first_active_field_line <number>`: The first visible line of a field
- `--lfll, --last-active-field-line, --last_active_field_line <number>`: The last visible line of a field  
- `--ffrl, --first-active-frame-line, --first_active_frame_line <number>`: The first visible line of a frame
- `--lfrl, --last-active-frame-line, --last_active_frame_line <number>`: The last visible line of a frame

#### Chroma Processing
- `--chroma-gain <number>`: Gain factor applied to chroma components (default 1.0)
- `--chroma-phase <number>`: Phase rotation applied to chroma components
- `--chroma-nr <number>`: NTSC: Chroma noise reduction
- `--luma-nr <number>`: Luma noise reduction level in IRE

#### NTSC Specific
- `-o, --oftest`: NTSC: Overlay the adaptive filter
- `--ntsc-phase-comp`: NTSC: Adjust phase per-line using pilot burst

#### Transform Decoder
- `--simple-pal`: Transform: Use 1D UV filter  
- `--transform-threshold <number>`: Transform: Uniform similarity threshold
- `--transform-thresholds <file>`: Transform: File containing per-line thresholds
- `--show-ffts`: Transform: Overlay the input and output spectra

#### Standard Options
- `-d, --debug`: Show debug messages
- `-q, --quiet`: Suppress info and warning messages

## Examples

```bash
# PAL LaserDisc (Transform Decoder)
ld-chroma-decoder -p transform input.tbc output.rgb

# NTSC LaserDisc (3D Decoder)
ld-chroma-decoder -n 3D input.tbc output.rgb

# Grayscale Output
ld-chroma-decoder -p mono input.tbc output.rgb

# Process Specific Range
ld-chroma-decoder -p transform -s 1000 -l 500 input.tbc output.rgb

# With Chroma Adjustments
ld-chroma-decoder -p transform --chroma-gain 1.2 --chroma-phase -2.0 input.tbc output.rgb

# Decode what a producer writes to a pipe, with no files (NTSC TBC, then CVBS from decode-orc)
producer | ld-chroma-decoder --system ntsc - output.rgb
orc-cli ... -o "CVBSSink=output_path=-" | ld-chroma-decoder --input-format cvbs --system pal - output.rgb
```

## Input/Output

### Input Format
- TBC files: 16-bit unsigned composite video
- Piped TBC fields or CVBS frames (see "Piped input"), read ahead on a thread of their own so that a slow or bursty producer does not hold the decoding up; if the input ends early the frames decoded so far are kept
- Metadata: SQLite format
- Standards: PAL (625-line), NTSC (525-line), PAL-M (525-line PAL)

### Output Format
- RGB: 16-bit per channel, interlaced or progressive
- Dimensions: Automatically determined from TBC metadata
- Frame rate: Original field rate (50/60 Hz) or frame rate (25/30 Hz)

## Troubleshooting

### Color Issues
- **Wrong colors**: Check chroma phase setting
- **Weak saturation**: Increase chroma gain
- **Color noise**: Try different decoder (Transform recommended for PAL)
- **Vertical color smearing**: Use 3D or Transform decoder

### Performance Issues
- **Slow processing**: Reduce thread count or use simpler decoder
- **High memory usage**: Process in smaller batches with -l option
