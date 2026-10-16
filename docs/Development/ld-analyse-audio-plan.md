# ld-analyse audio playback: plan and state

Branch `feature/ld-analyse-audio` (from `main` after the LDX merge). Notes for whoever continues, written so that the work can be picked up without the conversation it came from.

## The design (agreed with the author)

- A **waveform panel under the video**, toggled in `View > Waveform`; shown only when the capture has audio (a pcm track in its LDX file, the `.pcm` beside a `.tbc`, the `_audio_0.wav` of a CVBS capture, imported audio, or EFM audio that was decoded).
- **Left column of the panel** (audition style): source (`Analogue` / `EFM digital` / `Imported`), track of the LDX file when there are several, `L` and `R` switches (a channel alone plays on both sides), global volume. The column can be hidden, and the same controls are repeated in the `Audio config` window.
- **`Audio config` window**, three tabs:
  - `Output`: sound device, waveform colour, the controls repeated, the side panel switch.
  - `Import`: any file ffmpeg reads, cut to the length of the video, offset in ms; `Use` (session) or `Save in the file as a track` (`LdxCapture::addTrackFromFile`, a pcm track with `source: "import"`).
  - `EFM`: run the EFM decoder from the program, progressively. Two decoders exist and both are offered, with a live A/B comparison:
    - the suite `efm-decoder-f2 | efm-decoder-d24 | efm-decoder-audio --no-wav-header` (they read stdin and write stdout);
    - `ld-process-efm` (one step; it needs a `-` mode for stdin/stdout first).
    Decoding is progressive into a growing cache (a temporary PCM file), so playback and the waveform follow it; seeking is possible in what is decoded. It is the only way for a pipe. The EFM of an LDX stream must be passed on to the decoder (`LdxStream` only passes the video today). Options: no timecodes (default for a LaserDisc), error treatment. The two decoders are aligned on disc time for the A/B switch and for a difference strip on the waveform.
- **Playback rules**: audio plays only while the video plays (Play), it is the clock at 1x (the video takes its time from the sound played), and it is off at other speeds. Settings kept in `ld-analyse.ini` (`[audio]`). Static capture: the waveform is computed whole on a thread; live input: accumulate it like the SNR graphs.
- Output through **Qt Multimedia** (`QAudioSink`), optional: without it ld-analyse builds without audio (`LDDECODE_AUDIO_AVAILABLE`, `LDDECODE_HAVE_AUDIO`).

## Done

Step 1 (commit 663a742, plus d97acd2 for the pipe detection):

- Library (`src/library/tbc`): `AudioReader` (reads the audio as 16-bit stereo from an LDX pcm track, a `.pcm`, a WAV or memory; 24-bit brought to 16), `AudioPeaks` (peaks, thread-safe, add in pieces or all), `mixStereo` (L/R/volume); `LdxCapture::audioTracks`, `openAudio(name, error, wide, audioTrack)`, `getAudioBytes`, `audioBytesPerSample`. Test: `testaudio`.
- ld-analyse: `AudioEngine` (QAudioSink fed by a pull `AudioFeeder`, peaks thread, offset), `WaveformPanel`/`WaveformView`, `PlaybackController::setExternalClock` and `looped`, `Configuration::audioValue`, `View > Waveform`. Test: `testwaveform` (offscreen).
- Build: `vcpkg.json` (feature `qt` gains `qtmultimedia`), CI Qt installs (`modules: "qtmultimedia"`), flake. Always build with `build.bat`, never with a cmake run from another environment (the PATH changes the vcpkg ABI and qtbase is rebuilt, and fails); close every program run from `build\bin` first (their DLLs are locked).
- Unrelated fix on this branch: `ld-analyse` and `ld-chroma-decoder` tell an LDX stream from raw video by its first bytes, `--system` or not; `ld-analyse` live input takes the video parameters the stream sends.

## First feedback from a test (author, with a capture from ld-decode-rust)

1. The panel is ugly and too thick: make the waveform shorter (about 60 px), tidy the left column (the L/R buttons and the slider look raw), follow the theme.
2. The source menu must always be there (analogue vs EFM), even with one source, since EFM decoding will be started from it. Show the entries that are missing as disabled or as "Decode EFM...".
3. The waveform stayed empty on `D:\decodde\test.ldx` (written by ld-decode-rust): its pcm track has real audio (max sample about 10000) but the index of the file has one entry too many for track 3 (1463 in the index, 1462 blocks in the file, `ld-container verify`). `AudioEngine::selectTrack` fails (the last indexed block cannot be read) and the panel stays empty. Cause to find: duplicate or wrong cue in the file (compare the cue times with the block times; dedupe the index by position and not by time; skip an entry whose block is not of the track); also show the reason in the panel instead of an empty bar.

## To do, in order

1. DONE (points 1 to 3: thinner panel, source combo always shown with missing sources disabled, index of files from other writers: dedup by position and a last cue that cannot be read is dropped; `verify D:decodde	est.ldx` is sound). Still to do here: show the reason in the panel when a track cannot be opened.
2. DONE (needs a test by hand in the GUI): `Audio config` window (Output and Import tabs, ffmpeg through QProcess, offset, Use / Save in the file as a track via `addTrackFromFile`, source "import"); the panel shows the reason when a track cannot be opened. To check by hand: saving into an LDX file that ld-analyse has open (Windows may refuse the replace).
3. DONE except the live part and the difference strip: `ld-process-efm` reads "-" (raw or LDX stream) and writes "-"; `EfmDecoding` (src/ld-analyse) runs the suite as piped QProcesses or ld-process-efm, writes the audio as it comes into a cache file read by a growing `AudioReader` (`openGrowing`, `refresh`); `AudioEngine::addDecoded/extend/removeDecoded`; EFM tab of Audio config with the two decoders (each a track of the EFM source: the track menu is the A/B switch). Still to do: the EFM of an LDX stream passed to the decoder (step 4), the difference strip between the two decoders, disc-time alignment of the two.
4. Live input: waveform accumulated as the audio arrives, audio of a stream (the pcm track of the LDX stream).
5. Sync: use the `audioSamples` of the metadata for the analogue audio of a capture (exact) instead of time times rate.
6. Docs: `docs/Tools/ld-analyse.md` (waveform, Audio config, EFM tab), the how-to guide, the README line.

## Second round of feedback (author)

Done: mute button `M` (panel and window); waveform colours are in the theme (two more entries of the theme editor, "Waveform: analogue audio" and "Waveform: audio from EFM", kept in the palette as the visited-link colour, with defaults for light and dark; `waveformColour()` in `gui-common/theme.h`), the panel picks one by the source of the track played; the window is `Window > Audio panel...` (was `View > Audio config`), with a `Show the waveform` switch that follows `View > Waveform`; the waveform sits in a frame, and the main window grows or shrinks by its height when it is shown or hidden; tab `Style` with three designs (filled, outline, bars). To adjust after the author's test: more designs, spacing and look of the frame and of the left column.

Third round: the waveform has no background of its own (the application's, with a faint grid like the SNR plot of the stacker), L/R/M keep their frame (no auto-raise), three heights in `Style` (Compact by default, Large, Ultra compact: the controls in one row beside the waveform), and the theme editor scrolls (the waveform colours are the last two rows of its list). `WAVEFORM_PICTURES=<dir>` makes `testwaveform` save pictures of the three heights.
