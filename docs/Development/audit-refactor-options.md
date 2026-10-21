# Command line options of every tool (annex of the inventory, branch `audit-refactor`)

Read from the source (`QCommandLineOption` definitions in each `main.cpp`, `library/tbc/logging.cpp` for `-d` and `-q`, `library/tbc/metadataoptions.cpp` for the metadata
options). It is a listing for the work of domain N, not documentation: if an option is missing here, its definition is built in a way the reading did not catch (`ld-container`'s
sub-commands, `ld-export-metadata`, the two `ld-process-ac3` programs which use `getopt`, `ld-stacker-gui`'s own options). Those are listed in the inventory (N-03 to N-06).

Standard on most tools: `-h --help`, `-v --version`, `-d --debug`, `-q --quiet`; the metadata options `--meta`, `--input-metadata`, `--output-metadata` (`MetadataOptions`).

```
== ld-chroma-decoder/main.cpp
  --input-format <format>  | Piped input: ldx (an LDX stream, which says what it holds: the default when nothing else is said), tbc (fields) or cvbs (frames, as decode-orc writes them)
  --system <system>  | Piped input without metadata: the video system (pal, ntsc or pal-m)
  --black-level <number>  | Piped TBC input without metadata: the black16bIre of the capture (default: the standard level of the system)
  --white-level <number>  | Piped TBC input without metadata: the white16bIre of the capture (default: the standard level of the system)
  --subcarrier-locked <yes|no>  | Piped input without metadata: whether PAL was sampled locked to the subcarrier, which puts the second field 2 samples off (yes or no; default yes for CVBS, no for TBC)
  --sample-encoding <encoding>  | Piped CVBS input: u10 (CVBS_U10_4FSC, the default), u16, tpg21 or s16
  --buffer-frames <number>  | Piped input: frames read ahead of the one being decoded, and kept behind it (default 32)
  -s --start <number>  | Specify the start frame number
  -l --length <number>  | Specify the length (number of frames to process)
  -r --reverse  | Reverse the field order to second/first (default first/second)
  --chroma-gain <number>  | Gain factor applied to chroma components (default 1.0)
  --chroma-phase <number>  | Phase rotation applied to chroma components (degrees; default 0.0)
  -p --output-format  | 
  -b --blackandwhite  | Output in black and white
  --pad --output-padding <number>  | Pad the output frame to a multiple of this many pixels on both axes (1 means no padding, maximum is 32)
  -f --decoder <decoder>  | Decoder to use (pal2d, transform2d, transform3d, ntsc1d, ntsc2d, ntsc3d, ntsc3dnoadapt, mono; default automatic)
  -t --threads <number>  | Specify the number of concurrent threads (default number of logical CPUs)
  --ffll --first_active_field_line <number>  | The first visible line of a field. Range 1-259 for NTSC (default: 20), 2-308 for PAL (default: 22)
  --lfll --last_active_field_line <number>  | The last visible line of a field. Range 1-259 for NTSC (default: 259), 2-308 for PAL (default: 308)
  --ffrl --first_active_frame_line <number>  | The first visible line of a frame. Range 1-525 for NTSC (default: 40), 1-620 for PAL (default: 44)
  --lfrl --last_active_frame_line <number>  | The last visible line of a frame. Range 1-525 for NTSC (default: 525), 1-620 for PAL (default: 620)
  -o --oftest  | NTSC: Overlay the adaptive filter map (only used for testing)
  --chroma-nr <number>  | NTSC: Chroma noise reduction level in dB (default 0.0)
  --luma-nr <number>  | Luma noise reduction level in dB (default 0.0)
  --ntsc-phase-comp  | NTSC: Adjust phase per-line using burst phase
  --adapt-threshold <number>  | NTSC: 3D adaptive filter threshold (default 1.0, higher = more 3D)
  --chroma-weight <number>  | NTSC: Chroma weight for 3D adaptive filter (default 1.0, higher = more 2D)
  --simple-pal  | Transform: Use 1D UV filter (default 2D)
  --transform-threshold <number>  | Transform: Uniform similarity threshold (default 0.4)
  --transform-thresholds <file>  | Transform: File containing per-bin similarity thresholds
  --show-ffts  | Transform: Overlay the input and output FFTs
== ld-process-vbi/main.cpp
  -n --nobackup  | Do not create a backup of the input metadata
  -t --threads <number>  | Specify the number of concurrent threads (default is the number of logical CPUs)
== ld-process-vits/main.cpp
  -n --nobackup  | Do not create a backup of the input metadata
  -t --threads <number>  | Specify the number of concurrent threads (default is the number of logical CPUs)
== ld-dropout-correct/main.cpp
  -r --reverse  | Reverse the field order to second/first (default first/second)
  -o --overcorrect  | Over correct mode (use on heavily damaged single sources)
  -i --intra  | Force intrafield correction (default interfield)
  -t --threads <number>  | Specify the number of concurrent threads (default is the number of logical CPUs)
== ld-disc-stacker/main.cpp
  -? -h --help  | Displays help on commandline options.
  --help-mode  | Show info about stacking mode
  -V --verbose  | Show more info during stacking
  -r --reverse  | Reverse the field order to second/first (default first/second)
  -t --threads <number>  | Specify the number of concurrent threads (default is the number of logical CPUs)
  -m --mode <number>  | Specify the stacking mode to use (default is Auto) -1 = auto / 0 = mean / 1 = median / 2 = smart mean / 3 = smart neighbor / 4 = neighbor
  --st --smart-threshold <number>  | Specify the range of value in 8 bit (0~128) for selecting sample where the distance to the median didn't exceed the selected value for applying mean (default is 15)
  --no-diffdod  | Do not use differential dropout detection on low source pixels
  --no-map  | Disable mapping requirement
  --passthrough  | Pass-through dropouts present on every source
  --it --integrity  | Check if frames contain skip or sample drop and discard bad source for specific frame
== ld-discmap/main.cpp
  -r --reverse  | Reverse the field order to second/first (default first/second)
  -m --maponly  | Only perform mapping - No output TBC file required
  -s --nostrict  | No strict checking on pulldown frames
  -u --delete-unmappable-frames  | Delete unmappable frames (by default they are kept, given a number and marked as dropouts)
  -n --no-audio  | Do not process analogue audio
== ld-json-converter/main.cpp
  --input-json <filename>  | Specify the input JSON file
  --output-sqlite <filename>  | Specify the output SQLite file (default same as input but with .db extension)
== ld-sqlite-to-json/main.cpp
  --input-sqlite <filename>  | Specify the input SQLite file
  --output-json <filename>  | Specify the output JSON file (default same as input but with .json extension)
== ld-tbc-to-cvbs/main.cpp
  -e --encoding <encoding>  | Sample encoding: u10 (CVBS_U10_4FSC, default), u16, tpg21 or s16
  -r --samplerate <Hz>  | Sample rate of the audio in Hz (default: from the metadata, else 44100). The audio is copied as it is, not resampled
== ld-cvbs-to-tbc/main.cpp
  -r --samplerate <Hz>  | Sample rate of the audio in Hz (default: that of the WAV file, normally 48000). The audio is copied as it is, not resampled
== ld-lds-converter/main.cpp
  -i --input <file>  | Specify input laserdisc sample file (default is stdin)
  -o --output <file>  | Specify output laserdisc sample file (default is stdout)
  -u --unpack  | Unpack 10-bit data into 16-bit (default)
  -p --pack  | Pack 16-bit data into 10-bit
  -r --riff  | Unpack 10-bit data into 16-bit with RIFF WAV headers (use this ONLY for FlaCCL)
== ld-export-metadata/main.cpp
   <file>  | Write VITS information as CSV
   <file>  | Write VBI information as CSV
   <file>  | Write navigation information as Audacity labels
   <file>  | Write navigation information as FFMETADATA1
   <file>  | Write closed captions as Scenarist SCC V1.0 format
== ld-export-decode-metadata/main.cpp
  --input-sqlite <filename>  | Specify the input SQLite file
  --output-json <filename>  | Specify the output JSON file (default same as input but with -export.json extension)
== ld-chroma-decoder/encoder/main.cpp
  -f --system <system>  | Video system (PAL, NTSC; default PAL)
  -p --input-format  | 
  --field-offset <offset>  | Offset of the first output field within the field sequence (0, 2 for NTSC; 0, 2, 4, 6 for PAL; default: 0)
  --chroma-mode <chroma-mode>  | NTSC: Chroma encoder mode to use (wideband-yuv, wideband-yiq, narrowband-q; default: wideband-yuv)
  --no-setup  | NTSC: Output NTSC-J, without 7.5 IRE setup
  -c --sc-locked  | PAL: Output samples are subcarrier-locked (default: line-locked)
== ld-analyse/main.cpp
  --input-format <format>  | Piped input: ldx (an LDX stream, which says what it holds: the default when no video system is said), tbc (fields) or cvbs (frames, as decode-orc writes them)
  --system <system>  | Piped input: the video system (pal, ntsc or pal-m)
  --black-level <number>  | Piped TBC input: the black16bIre of the capture (default: the standard level of the system)
  --white-level <number>  | Piped TBC input: the white16bIre of the capture (default: the standard level of the system)
  --subcarrier-locked <yes|no>  | Piped input: whether PAL was sampled locked to the subcarrier, which puts the second field 2 samples off (yes or no; default yes for CVBS, no for TBC)
  --sample-encoding <encoding>  | Piped CVBS input: u10 (CVBS_U10_4FSC, the default), u16, tpg21 or s16
  --buffer-frames <number>  | Piped input: the size of each of the two buffers, in frames (default: 5 seconds of video). A paused input can be gone through in the frozen one while the other goes on filling
  --throttle  | 
== ld-process-efm/main.cpp
  -c --conceal  | Conceal corrupt audio data (default)
  -s --silence  | Silence corrupt audio data
  -g --pass-through  | Pass-through corrupt audio data
  -p --pad  | Pad start of audio from 00:00 to match initial disc time
  -b --data  | Decode F1 frames as data instead of audio
  -D --dts  | Audio is DTS rather than PCM (allow non-standard F3 syncs)
  -t --time  | Non-standard audio decode (no time-stamp information)
  --debug-efmtof3frames  | Show EFM To F3 frame decode detailed debug
  --debug-syncf3frames  | Show F3 frame synchronisation detailed debug
  --debug-f3tof2frames  | Show F3 To F2 frame decode detailed debug
  --debug-f2tof1frame  | Show F2 to F1 frame detailed debug
  --debug-f1toaudio  | Show F1 to audio detailed debug
  --debug-f1todata  | Show F1 to data detailed debug
== efm-decoder/tools/*/src/main.cpp (read by eye, the script does not read these)
  efm-decoder-f2    --no-timecodes  --show-f3 --show-f2  --show-tvalues-debug --show-channel-debug --show-f3-debug --show-f2-correct-debug --show-all-debug
  efm-decoder-d24   --no-timecodes  --show-f1 --show-data24 --show-f2-debug --show-f1-debug --show-all-debug
  efm-decoder-audio --no-timecodes  --audacity-labels --no-audio-concealment --zero-pad --no-wav-header --show-audio --show-audio-debug --show-audio-correction-debug --show-all-debug
  efm-decoder-data  --no-timecodes  --output-metadata --show-rawsector --show-rawsector-debug --show-sector-debug --show-sector-correction-debug --show-all-debug
```
