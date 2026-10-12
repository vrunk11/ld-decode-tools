# ld-analyse

**TBC File Analysis and Visualization GUI**

## Overview

ld-analyse is a graphical user interface (GUI) tool for analyzing and visualizing Time Base Corrected (TBC) LaserDisc video files produced by ld-decode. It provides comprehensive analysis tools for signal quality assessment, dropout detection, VBI metadata viewing, and real-time video preview.

## Usage

### Command Line
```bash
ld-analyse [options] <input.tbc>
```

### GUI Operation

1. **Open File**: File → Open file (a `.tbc` or `.cvbs` file), or specify on command line
2. **Navigate**: Use media controls to move between frames/fields
   - Previous/Next buttons
   - Slider for quick navigation
   - Frame number entry
   - Start/End buttons to jump to beginning/end
3. **View Options**: Click buttons to toggle
   - Video: Enable/disable video display
   - Aspect: Toggle aspect ratio correction
   - Dropouts: Highlight dropout regions
   - Sources: Compare multiple sources (when available)
   - View: Switch between frame/field/split modes
   - Field Order: Toggle first/second field first
4. **Analysis Tools**: Tools menu
   - Line Scope: Waveform oscilloscope
   - Vectorscope: Color analysis
   - VBI: Metadata display
   - Dropout Analysis: Statistical dropout information
   - SNR Analysis: Signal-to-noise measurements
5. **Save**: File → Save frame as PNG

## Options

#### Common Options
- `-h, --help`: Display help on command-line options
- `-v, --version`: Display version information
- `-d, --debug`: Show debug information
- `-q, --quiet`: Suppress info and warning messages

#### Display Options
- `--force-dark-theme`: Force dark theme regardless of system settings

#### Live input (the input is `-`)
Video piped in has no metadata, so the system must be given. See "Live input" below.
- `--system <pal|ntsc|pal-m>`: The video system (required)
- `--input-format <tbc|cvbs>`: Fields of TBC (the default) or frames of CVBS
- `--black-level`, `--white-level`: TBC levels (default: the standard ones)
- `--subcarrier-locked <yes|no>`, `--sample-encoding <u10|u16|tpg21|s16>`
- `--buffer-frames <number>`: The size of each of the two buffers, in frames (default: 5 seconds of video)
- `--throttle`: Hold the producer back to the pace of the viewer instead of overwriting old video (for a file, or a decoder that is not live)

#### Arguments
- `input`: Input TBC or CVBS file to analyze, or `-` for video piped in
- `-v, --version`: Display version information

## Examples

```bash
# Open a TBC (or CVBS) file for analysis
ld-analyse capture.tbc
```

## Playback, video filters and live input

- **Playback**: the play button, the Playback menu and the space bar play the video at its real speed (0.25x to 4x, optionally looping). Frames are decoded ahead by several threads and skipped, rather than slowed down, if the machine cannot keep up. The scope and the other tools follow the frame shown.
- **Video filters**: Window > Video filter... runs the picture through ffmpeg filters (libavfilter): a deinterlacer (yadif, bwdif, w3fdif, estdif), a scaling algorithm, and any other filters, each with a check box. The view button also has a Deinterlaced mode. They change only the picture shown, not the data the analysis tools read. The Filters button (or Window > Video filters on) switches them all on or off at once, to compare. Built with libavfilter by default (`USE_FFMPEG=0` in build.bat leaves it out).
- **Live input**: `producer | ld-analyse - --system ntsc` (add `--input-format cvbs` for CVBS) shows video as it arrives, with no length limit. Playback starts by itself at the newest frame and stays on, waiting for more video at the end of what has arrived; it can not be sought while it plays. Paused, the last 5 seconds or so are frozen and the slider and buttons go through them, for analysing a sequence, while the input goes on filling a second buffer (it is never held up); playing again carries on into it. Dropout, SNR and VBI data come from metadata, which a pipe does not have.

## Input/Output

### Input
- `.tbc` files with associated `.tbc.db` metadata
- Supports PAL (625-line, 50 Hz), NTSC (525-line, 60 Hz), and PAL-M (525-line PAL variant)

### Output
- PNG images (frame export)
- Analysis reports and statistics

## Troubleshooting

### Performance Issues
- Toggle chroma decoder off during seeking for faster navigation
- Reduce zoom level if display is slow
- Check that Qt5 GPU acceleration is available

### Analysis Issues
- Use the oscilloscope to verify signal levels and timing
- Check the vectorscope to ensure correct color burst phase
- Monitor dropout analysis to identify disc damage patterns
- Compare SNR across the disc to find optimal playback regions
- Use VBI data to verify frame numbers and chapter markers
