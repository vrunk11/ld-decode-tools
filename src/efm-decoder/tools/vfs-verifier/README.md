# vfs-verifier

**Acorn VFS (Domesday) image verifier**

## Overview

vfs-verifier checks a data image recovered from an EFM capture of a Domesday LaserDisc (an Acorn ADFS image, written by `efm-decoder-data`) against the **bad sector map** that `efm-decoder-data` writes beside it. It reads the free space map and the root directory of the image, and for every file of the directory it looks at the sectors the file uses: any of them that the map says was bad is reported, with a hex dump of the sector.

## Usage

```bash
vfs-verifier [options] <input> <bad-sector-map>
```

### Arguments

- `input` - the image written by `efm-decoder-data`
- `bad-sector-map` - the bad sector map written by `efm-decoder-data --output-metadata` (named after the output: `.dat` becomes `.bsm`, or `.bsm` is added)

### Options

- `-h, --help` - Display help information
- `-v, --version` - Display version information
- `-d, --debug` - Show debug output (the directory entries, one by one)
- `-q, --quiet` - Suppress info and warning messages

## Result

- A warning for each bad EFM sector found in a file (the file name, the ADFS sector, a hex dump of it).
- The last message says whether the verification passed (no bad sector in the files of the directory) or failed (and how many bad sectors were found).
- Exit code 0 when the verification could be done (whatever its result), 2 for a wrong command line, 1 when a file cannot be read.

## Example

```bash
efm-decoder-f2 domesday.efm domesday.f2
efm-decoder-d24 domesday.f2 domesday.d24
efm-decoder-data --output-metadata domesday.d24 domesday.dat

vfs-verifier domesday.dat domesday.bsm
```
