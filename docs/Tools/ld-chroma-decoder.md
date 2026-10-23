## ld-chroma-decoder


This application takes the output of ld-decode (a PAL or NTSC TBC file) and performs chroma decoding (i.e. colourises it).

The output is a sequence of RGB48, YUV444P16, or GRAY16 video frames suitable for piping to an external application such as FFmpeg.

Syntax:

`ld-chroma-decoder [options] <input TBC file name> [output file name]`

A capture whose metadata keep a map (ld-discmap `--save-map`) is decoded in the order of the map: the frames of the disc, the missing ones blank. `--ignore-map` decodes the capture as it is.

The input can also be a `.stack` stacking session: the stack of its sources is made as the frames are decoded, with the same result as stacking to a file with ld-disc-stacker and decoding that. The sources are lined up by the numbers of the disc that their VBI gives (then by their offsets), the stack goes from the earliest frame of any source to the latest of any, and a frame that no source has is a padded one. A source that has a mapping beside it (name_map, or the track of an LDX file that is its mapping) is read from the mapping unless its `followMap` is off in the session, one that has none as it is. The metadata is that of the first source that has each frame (without dropouts or VITS measurements).

> [!NOTE]
> This is mostly automated with FFmpeg profiles today with [tbc-video-export](https://github.com/JuniorIsAJitterbug/tbc-video-export#readme).


## Direct Command Example:


`.tbc` --> Chroma-Decoder y4m --> Pipe | --> FFmpeg --> V210 4:2:2 Uncompressed in the QuickTime MOV container.

--------------

NTSC

    ld-chroma-decoder --decoder ntsc3d -p y4m -q INPUT.tbc| ffmpeg -i - -c:v v210 -f mov -top 1 -vf setfield=tff -flags +ilme+ildct -pix_fmt yuv422p10le -color_primaries smpte170m -color_trc bt709 -colorspace smpte170m -color_range tv -vf setdar=4/3,setfield=tff OUTPUT.mov

PAL

    ld-chroma-decoder --decoder transform3d -p y4m -q INPUT.tbc| ffmpeg -i - -c:v v210 -f mov -top 1 -vf setfield=tff -flags +ilme+ildct -pix_fmt yuv422p10le -color_primaries bt470bg -color_trc bt709 -colorspace bt470bg -color_range tv -vf setdar=4/3,setfield=tff OUTPUT.mov


## Example with full vertical VBI data:


--------------------

Y4M Only.

NTSC

    ld-chroma-decoder --ffll 1 --lfll 259 --ffrl 2 --lfrl 525 --decoder ntsc3d -p y4m -q INPUT.tbc OUTPUT.mov

PAL

    ld-chroma-decoder --ffll 2 --lfll 308 --ffrl 2 --lfrl 620 --decoder transform3d -p y4m -q INPUT.tbc OUTPUT.mov


## Notes & Command Argument List


Most options have a corresponding setting in ld-analyse's 'Chroma decoder configuration' window so you can experiment with them interactively there. For more information, see the [ld-analyse manual](https://github.com/happycube/ld-decode/wiki/ld-analyse#chroma-decoder-configuration).

The `mono` filter treats the whole signal as luma, so it's the best choice when you know that the input video doesn't contain any colour information (e.g. black-and-white films).


# Output Framing Control


### First Active Field Line


`--first-active-field-line int`, `--ffll int`

The name with underscores (`--first_active_field_line`) works too; so do the ones of the three options that follow (`--last_active_field_line`, `--first_active_frame_line`, `--last_active_frame_line`).

The first visible line of a field.

Ranges:

| Format | Range   | Default |
| ------ | ------- | ------- |
| NTSC   | 1 - 259 | 20      |
| PAL    | 2 - 308 | 22      |


### Last Active Field Line


`--last-active-field-line int`, `--lfll int`

The last visible line of a field.

Ranges:

| Format | Range   | Default |
| ------ | ------- | ------- |
| NTSC   | 1 - 259 | 259     |
| PAL    | 2 - 308 | 308     |


### First Active Frame Line


`--first-active-frame-line int`, `--ffrl int`

The first visible line of a frame.

Ranges:

| Format | Range   | Default |
| ------ | ------- | ------- |
| NTSC   | 1 - 525 | 40      |
| PAL    | 1 - 620 | 44      |


### Last Active Frame Line


`--last-active-frame-line int`, `--lfrl int`

The last visible line of a frame.


Ranges:

| Format | Range   | Default |
| ------ | ------- | ------- |
| NTSC   | 1 - 525 | 525     |
| PAL    | 1 - 620 | 620     |


# Chroma Decoding Control


### Decoder


`-f decoder`, `--decoder decoder`

Chroma decoder to use. Without it, the decoder is the one **kept in the metadata of the capture** (see "Decoding parameters" below) if there is one; if not, it is chosen from the source as ld-analyse chooses it: one capture in **3D** (`transform3d` for PAL and PAL-M, `ntsc3d` for NTSC), a stack of LaserDiscs (a `.stack` session) in 3D too, a stack of tapes (a tape format in the metadata, or captures made by vhs-decode) in 2D (`transform2d`, `ntsc2d`), and a stream in 2D. For an NTSC capture from a tape (a tape format in the metadata, or made by vhs-decode) the phase compensation is turned on as well. The log says which decoder is used and why. (It used to be `pal2d` and `ntsc2d`: give `--decoder pal2d` or `--decoder ntsc2d` for that.)

**Decoding parameters.** The metadata of a capture can keep how it was chosen to be decoded: the decoder and its settings (chroma gain and phase, luma and chroma noise reduction, the adaptive filter thresholds, the phase compensation, the simple PAL decoder). ld-analyse writes them (*Config > Keep the decoder settings for this capture*). They are the default of ld-chroma-decoder for the capture: what is given on the command line has the last word.

Available decoders:

| Format | Decoder         | Default   |
| ------ | --------------- | --------- |
| B/W    | `mono`          |           |
| PAL    | `pal2d`         | PAL/PAL-M |
| PAL    | `transform2d`   |           |
| PAL    | `transform3d`   |           |
| NTSC   | `ntsc1d`        |           |
| NTSC   | `ntsc2d`        | NTSC      |
| NTSC   | `ntsc3d`        |           |
| NTSC   | `ntsc3dnoadapt` |           |


### Chroma Gain


`--chroma-gain 0`

Gain factor applied to chroma components normally you will not adjust this over `1~3` in value.

### Chroma Phase


`--chroma-phase float`

Phase rotation applied to chroma components (degrees `0~360`).


### Luma NR


`--luma-nr float`

Luma noise reduction level in IRE.

>[!CAUTION]
> It is unlikely you need to adjust this.
> We recommend applying noise reduction in post via avisynth. vapoursynth or tools like Resolve.


# Help Page


```
Options:
  -?, -h, --help                              Displays help on commandline
                                              options.
  --help-all                                  Displays help, including generic
                                              Qt options.
  -v, --version                               Displays version information.
  -d, --debug                                 Show application debug messages
  -q, --quiet                                 Suppress info and warning
                                              messages
  --meta, --metadata-format <db|json>         Metadata format to use: db
                                              (SQLite <input>.db) or json
                                              (<input>.json). Default: db if
                                              present, otherwise json
  --input-metadata <filename>                 Specify the input metadata file
                                              (default input.db or input.json,
                                              see --meta)
  --input-format <format>                     Piped input: tbc (fields, the
                                              default) or cvbs (frames, as
                                              decode-orc writes them)
  --system <system>                           Piped input without metadata: the
                                              video system (pal, ntsc or pal-m)
  --black-level <number>                      Piped TBC input without metadata:
                                              the black16bIre of the capture
                                              (default: the standard level of
                                              the system)
  --white-level <number>                      Piped TBC input without metadata:
                                              the white16bIre of the capture
                                              (default: the standard level of
                                              the system)
  --subcarrier-locked <yes|no>                Piped input without metadata:
                                              whether PAL was sampled locked to
                                              the subcarrier, which puts the
                                              second field 2 samples off (yes or
                                              no; default yes for CVBS, no for
                                              TBC)
  --sample-encoding <encoding>                Piped CVBS input: u10
                                              (CVBS_U10_4FSC, the default), u16,
                                              tpg21 or s16
  --buffer-frames <number>                    Piped input: frames read ahead of
                                              the one being decoded, and kept
                                              behind it (default 32)
  -s, --start <number>                        Specify the start frame number
  -l, --length <number>                       Specify the length (number of
                                              frames to process)
  -r, --reverse                               Reverse the field order to
                                              second/first (default
                                              first/second)
  --ignore-map                                Read the capture as it is,
                                              though its metadata keep a
                                              map (the frames in the order
                                              of the disc, from ld-discmap)
  --chroma-gain <number>                      Gain factor applied to chroma
                                              components (default 1.0)
  --chroma-phase <number>                     Phase rotation applied to chroma
                                              components (degrees; default 0.0)
  -p, --output-format <output-format>         Output format (rgb, yuv, y4m;
                                              default rgb); RGB48, YUV444P16,
                                              GRAY16 pixel formats are supported
  -b, --blackandwhite                         Output in black and white
  --pad, --output-padding <number>            Pad the output frame to a
                                              multiple of this many pixels on
                                              both axes (1 means no padding,
                                              maximum is 32)
  -f, --decoder <decoder>                     Decoder to use (pal2d,
                                              transform2d, transform3d, ntsc1d,
                                              ntsc2d, ntsc3d, ntsc3dnoadapt,
                                              mono; default automatic)
  -t, --threads <number>                      Specify the number of concurrent
                                              threads (default number of logical
                                              CPUs)
  --ffll, --first-active-field-line, --first_active_field_line <number>  The first visible line of a
                                              field. Range 1-259 for NTSC
                                              (default: 20), 2-308 for PAL
                                              (default: 22)
  --lfll, --last-active-field-line, --last_active_field_line <number>   The last visible line of a field.
                                              Range 1-259 for NTSC (default:
                                              259), 2-308 for PAL (default: 308)
  --ffrl, --first-active-frame-line, --first_active_frame_line <number>  The first visible line of a
                                              frame. Range 1-525 for NTSC
                                              (default: 40), 1-620 for PAL
                                              (default: 44)
  --lfrl, --last-active-frame-line, --last_active_frame_line <number>   The last visible line of a frame.
                                              Range 1-525 for NTSC (default:
                                              525), 1-620 for PAL (default: 620)
  -o, --oftest                                NTSC: Overlay the adaptive filter
                                              map (only used for testing)
  --chroma-nr <number>                        NTSC: Chroma noise reduction
                                              level in IRE (default 0.0)
  --luma-nr <number>                          Luma noise reduction level in IRE
                                              (default 0.0)
  --ntsc-phase-comp                           NTSC: Adjust phase per-line using
                                              burst phase
  --adapt-threshold <number>                  NTSC: 3D adaptive filter
                                              threshold (default 1.0, higher =
                                              more 3D)
  --chroma-weight <number>                    NTSC: Chroma weight for 3D
                                              adaptive filter (default 1.0,
                                              higher = more 2D)
  --simple-pal                                Transform: Use 1D UV filter
                                              (default 2D)
  --transform-threshold <number>              Transform: Uniform similarity
                                              threshold (default 0.4)
  --transform-thresholds <file>               Transform: File containing
                                              per-bin similarity thresholds
  --show-ffts                                 Transform: Overlay the input and
                                              output FFTs

Arguments:
  input                                       Specify input TBC file (- for
                                              piped input)
  output                                      Specify output file (omit or -
                                              for piped output)
```

## LDX files

An LDX file (see [the container format](../Specifications/container-format.md)) can be opened where a TBC file is: `capture.ldx` is the capture it holds (or the stack of the session it keeps), and `capture.ldx#3` is the track 3 of a file that holds several captures. The fields and the metadata are read from the file, and nothing is written to it.

## LDX stream

With `-` and no `--input-format` or input metadata, an input that begins as a Matroska file does is an LDX stream (`--system` does not stop that) (`ld-container pack capture.tbc -o - | ld-chroma-decoder - out.rgb`): it says by itself what it holds, so the video system, TBC or CVBS and the metadata are taken from it. `--input-format ldx` says so. A stream that is not LDX is refused with a message that says how to read raw video.
