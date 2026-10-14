## ld-stacker-gui
A GUI for [ld-disc-stacker](ld-disc-stacker.md), in the style of ld-analyse: it shows the captures of a disc next to their stack, so that the difference in quality can be seen, and writes the stack.

```
ld-stacker-gui [files...]
```

Give it TBC files, or a `.stack` session file saved earlier, or drop them on the window. The sources must all be TBC (CVBS and TBC are not mixed) and of one video system: PAL and NTSC cannot be stacked together, and a source of the other one is refused.

### Window

- **Sources** (left): one row for each source. Drag a row to reorder the sources; the check box leaves a source out of the stack without removing it; the offset is a number of frames to shift the source by, to line it up with the others where mapping is not possible; the SNR button opens a window with the SNR of the source over the whole disc (one window for each source). The first source that is on is the reference: the stack is as long as it is and its offset means nothing. Right click, or the Delete key, removes a source.
- **Stack button and tabs**: one tab for each source, with the SNR of the frame being viewed (the best source in green), and and, in the row of buttons under the picture beside Chroma, the Stack button, which says the SNR of the stack. Stack on: the picture is the stack. Stack off, or a tab clicked: it is the source of that tab. Switching the button compares the stack with the source, at the same zoom and position. The keys 1 to 9 pick a source and 0 (or S) switches the stack.
- **Picture**: the first source imported is shown at 100% and the window is resized to it; those after it change nothing. Zoom by typing a value, with the + and - buttons (they go through 100%) or with Ctrl and the wheel, and drag the picture to pan. The Original size button (the last icon of the row, with the zoom ones) goes back to 100% and resizes the window to show the whole picture. The aspect ratio button switches between the display aspect ratio of the video (DAR 4:3 or 16:9, as ld-analyse shows it, the default) and square pixels (SAR 1:1). Line doubling enlarges the picture without smoothing it, which is what shows the difference between sources; Smooth is the other choice (View > Enlarge the picture). The Chroma button decodes the picture to colour; View > Chroma decoder configuration opens the configuration of the decoder (PAL filter, NTSC 1D, 2D or 3D, gain, phase, noise reduction). A 3D decoder looks at the frames either side, so for the stack those are stacked too. View > Enhance changes the brightness, contrast and gamma of what is shown, to bring out the defects, and nothing that is stacked.
- **SNR graph** under the picture: the white SNR (blue) and the black PSNR (orange) of what is shown, over the whole disc, with a marker on the frame; the two check boxes above it switch each curve. The values are in dB: the scale is written on the right of the graph, and the values at the cursor are read in a box that follows it. For a source it is the SNR of its metadata. The stack has none, so it is measured in the background from the stacked test signal lines as soon as the sources are loaded or changed (it takes a few seconds, and the graph says how far it is). Click the graph to go to a frame. The SNR window of a source has the same two curves; a check box adds those of the stack, in grey, to compare.
- **Frame navigation**: first, previous, next and last frame, the frame number, a slider; the left and right keys step.
- **Frame, split or field**: a list beside the aspect ratio button (key F goes through it) chooses what the picture is: Frame, Split (both fields of the frame, the first above the second), or Field 1 or Field 2 (one field with its lines doubled). The navigation is by frame in all of them, and the choice is kept in the session.
- **Stacking mode**: auto or one of the modes of ld-disc-stacker. In auto the status bar says which one the number of sources gives. Edit > Stacking options sets the rest: the threshold of the smart modes, differential dropout detection, pass-through, the integrity check (used when processing to a file) and the field order.
- **Status bar**: how many sources contribute to the frame, the mode, and what is shown.
- **View > Stack output**: a window with the stack, which stays in view while the main window shows a source.
- **View > Theme and Style**: the colour themes and widget styles of ld-analyse (Auto, Light, Dark, Soft light, Dim, Dark grey, and a custom theme to edit with Customize...), kept between sessions. `--force-dark-theme` forces the dark theme for one session.

The stack shown is made with the same code as ld-disc-stacker, for the frame viewed only.

### Menu

- **File > Open session** and **Export session** load and save a `.stack` file: a JSON file with the sources (file, on or off, offset), the stacking options, the output file and what the window shows. ld-disc-stacker takes it as it is (see its page), so a session made here can be stacked from the command line.
- **File > Process** asks where to save the stack and the metadata format (SQLite or JSON), then stacks the sources that are on. The video is TBC or CVBS as the extension of the file says; when that is not the format of the sources, ld-tbc-to-cvbs or ld-cvbs-to-tbc converts the stack, and ld-sqlite-to-json or ld-json-converter converts the metadata if it is not in the format asked for. The tools are those beside ld-stacker-gui. A stack that does not finish is removed.

The session can be opened in ld-analyse, which shows the stack as the frames are asked for, and decoded by ld-chroma-decoder, without the stack being written first.

### Not yet

Matching sources by VBI frame number (mapping) in the GUI.
