# ld-discmap

**TBC and VBI Alignment and Correction**

## Overview

ld-discmap performs TBC and VBI alignment and correction. It maps frame numbers from VBI data to the actual video fields, corrects field ordering issues, handles pulldown detection, and can delete unmappable frames. This tool is essential for ensuring proper frame synchronization and handling various video format conversions.



## Usage

### Basic Syntax
```bash
ld-discmap [options] <input.tbc> <output.tbc>
```

## Options

#### Common Options
- `-h, --help`: Display help on command-line options
- `-v, --version`: Display version information
- `-d, --debug`: Show debug information
- `-q, --quiet`: Suppress info and warning messages

#### Input/Output
- `input`: Input TBC file (required)
- `output`: Output TBC file (required unless using --maponly)

#### Processing Options
- `-r, --reverse`: Reverse the field order to second/first (default first/second)
- `-m, --maponly`: Only perform mapping - No output TBC file required
- `-s, --nostrict`: No strict checking on pulldown frames
- `-u, --delete-unmappable-frames`: Delete unmappable frames (by default they are kept, given a number and marked as dropouts: see below)
- `-n, --no-audio`: Do not process analogue audio (for a CVBS capture this is the `capture_audio_0.wav` file, which is mapped like the `.pcm` of a TBC capture, and skipped if it does not exist)
- `--meta <db|json>`: Metadata format to use when both input.db and input.json exist (default db, with a warning)

#### Verbosity
- `-q, --quiet`: Suppress info and warning messages
- `-d, --debug`: Show debug

## Examples

```bash
# Basic Frame Mapping
ld-discmap input.tbc output.tbc

# Map Only (No Output)
ld-discmap -m input.tbc output.tbc

# Delete Unmappable Frames
ld-discmap -u input.tbc output.tbc

# No Strict Pulldown Checking
ld-discmap -s input.tbc output.tbc

# Reverse Field Order
ld-discmap -r input.tbc output.tbc

# Skip Audio Processing
ld-discmap -n input.tbc output.tbc
```

## Input/Output

### Input Format
- TBC files with VBI metadata processed
- Associated SQLite metadata
- Supports all video standards (PAL, NTSC, PAL-M)

### Output Format
- Corrected TBC with updated metadata
- Frame mapping information in metadata

## Troubleshooting

### VBI mapping errors
- Ensure ld-process-vbi was run first
- Check that VBI data is present in metadata
- Some discs have poor VBI data quality

### Unmappable frames
- A frame that has no number and cannot be given one is kept, and the mapping does not fail: it takes the next free number after the frame before it, where there is room before the number of the frame after it (so it stays where it was and nothing shifts), and is written with a dropout over the whole of each field, so that the stack leaves it out. A frame with no room takes the number 0 and goes before the first frame, as a lead-in does
- `--delete-unmappable-frames` deletes them instead (their place is then padded like any other gap)
- The log says how many there were; check the result in ld-analyse
- Check source disc quality

### Field order issues
- Try --reverse option
- Verify metadata has correct field order
- Check with ld-analyse

### Audio sync problems
- Ensure audio processing is enabled (remove -n)
- Check that analogue audio is present
- Verify audio track alignment
