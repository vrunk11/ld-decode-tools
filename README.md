# ld-decode Tools

This is the complete suite of tools for processing LaserDisc captures and TBC (Time Base Corrected) files. The ld-decode project provides professional-grade tools for digitizing, processing, and analyzing analog video sources with exceptional quality and accuracy.

## Tool Categories

### Core Processing Tools
- **ld-process-vbi** - Decode Vertical Blanking Interval data
- **ld-process-vits** - Process Vertical Interval Test Signals
- **ld-process-efm** - Decode the EFM digital audio or data in a single step
- **ld-ac3-demodulate / ld-ac3-decode** (ld-process-ac3) - Extract Dolby Digital AC3 audio tracks

### EFM Decoder Suite
*Staged EFM decoding with stacking capabilities; an alternative to ld-process-efm*
- **efm-decoder-f2** - Convert EFM T-values to F2 sections
- **efm-decoder-d24** - Convert F2 sections to Data24 format
- **efm-decoder-audio** - Convert EFM Data24 sections to 16-bit stereo PCM audio
- **efm-decoder-data** - Convert EFM Data24 sections to ECMA-130 binary data
- **efm-stacker-f2** - Combine multiple F2 captures for improved quality
- **vfs-verifier** - Verify the file system of data recovered from Domesday (VFS) discs

### Analysis and Quality Tools
- **ld-analyse** - GUI tool for TBC file analysis and visualization
- **ld-discmap** - TBC and VBI alignment and correction tool
- **ld-dropout-correct** - Advanced dropout detection and correction
- **ld-chroma-decoder** - Color decoder for TBC LaserDisc video to RGB/YUV conversion
- **ld-chroma-encoder** - Encode RGB/YCbCr video into a TBC (used to test the decoder)
- **ld-disc-stacker** - Combine multiple TBC captures for improved quality

### Export and Conversion Tools
- **ld-export-metadata** - Export TBC metadata to external formats (VBI/VITS CSV, Audacity labels, FFMETADATA, SCC closed captions)
- **ld-export-decode-metadata** - Export TBC metadata to a versioned JSON format for external tools
- **ld-lds-converter** - Convert between 10-bit and 16-bit LaserDisc sample formats
- **ld-json-converter** - Convert JSON metadata (`.tbc.json`) to SQLite (`.tbc.db`)
- **ld-sqlite-to-json** - Convert SQLite metadata (`.tbc.db`) to JSON (`.tbc.json`)

### Utility Scripts
- **ld-compress** - Compress `.lds` captures to `.ldf` (FLAC through ffmpeg), uncompress them back, and verify them (in scripts/)
- **pcm2wav** - Wrap the 44.1 kHz 16-bit stereo PCM audio of a decode in a WAV file (in scripts/)

Both are bash scripts using ffmpeg; they are installed on Linux and macOS. `scripts/` also holds the
drivers of the functional tests, `test-chroma` and `test-decode-pretbc` (see [TESTING.md](TESTING.md)).

## Building

The tools build with CMake against Qt 6 and FFTW3. See [BUILD.md](BUILD.md):
- `nix develop`, then `./build.sh` (Linux/macOS), for the reproducible Nix environment
- `build.bat` (Windows), which fetches every dependency through a project-local vcpkg

Ready-made packages for Linux, macOS and Windows are built by the CI for every push and attached to
each GitHub release.

## Getting Started

1. **Capture Processing**: Start with [ld-decode](https://github.com/happycube/ld-decode) to convert raw RF captures to TBC format
2. **Quality Analysis**: Use `ld-analyse` to assess capture quality and identify issues
3. **Correction**: Apply `ld-dropout-correct` for dropout repair if needed
4. **Chroma Decoding**: Process composite sources with `ld-chroma-decoder`
5. **Export**: Convert to final formats with an external tool such as [tbc-video-export](https://github.com/JuniorIsAJitterbug/tbc-video-export)

## Important Notes

- **Metadata formats**: All tools read and write both SQLite (`.tbc.db`, written by current ld-decode) and JSON (`.tbc.json`, used by existing captures and other decoders) - both formats are fully supported
  - For `capture.tbc` the tools use `capture.tbc.db`, or `capture.tbc.json` if that is the only one present. When both exist the `.db` is used and a warning says so; `--meta json` (or `--meta db`) selects the source explicitly
  - Output metadata is always written in the same format as the input (JSON in, JSON out). The processing tools never convert between formats and refuse an output file in the other format; use `ld-json-converter` or `ld-sqlite-to-json` to convert
  - Updating metadata in place when both files exist warns that the other one is now out of date
  - The original `--input-json` / `--output-json` options are still accepted (hidden from `--help`) as aliases of `--input-metadata` / `--output-metadata`
- **File Extensions**: TBC files use `.tbc` extension, metadata uses `.tbc.db` (SQLite) or `.tbc.json` (JSON)
- **Dependencies**: Qt 6 and FFTW3; FFmpeg is only needed for the chroma tests and to turn the decoded output into video files
- **Performance**: Many tools support multi-threading for faster processing

> [!WARNING]  
> The SQLite metadata format is **internal to ld-decode tools only** and subject to change without notice. External tools and scripts should **not** access this database directly. Instead, use `ld-export-metadata` or `ld-export-decode-metadata` to export metadata in stable, documented formats.

## Documentation

Each tool directory under `src/` contains a README.md with usage instructions, the complete option
reference, examples and input/output formats. [docs/](docs/) has per-tool pages and how-to guides
(working with multiple discs, subtitles, creating video from NTSC and PAL decodes, ...).

Development: [BUILD.md](BUILD.md), [INSTALL.md](INSTALL.md), [TESTING.md](TESTING.md),
[CONTRIBUTING.md](CONTRIBUTING.md) and [AGENTS.md](AGENTS.md).

