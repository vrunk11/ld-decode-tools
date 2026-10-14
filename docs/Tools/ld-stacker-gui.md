## ld-stacker-gui
A GUI for [ld-disc-stacker](ld-disc-stacker.md), in the style of ld-analyse: it shows the captures of a disc next to their stack, so that the difference in quality can be seen, and writes the stack.

```
ld-stacker-gui [files...]
```

Give it TBC files, or a `.stack` session file saved earlier, or drop them on the window. The sources must all be TBC (CVBS and TBC are not mixed) and of one video system: PAL and NTSC cannot be stacked together, and a source of the other one is refused.

### Window

- **Sources** (left): one row for each source. Drag a row to reorder the sources; the check box leaves a source out of the stack without removing it; the offset is a number of frames to shift the source by, to line it up with the others where mapping is not possible; the SNR button opens a window with the SNR of the source over the whole disc, with that of the stack behind it. The first source that is on is the reference: the stack is as long as it is and its offset means nothing. Right click, or the Delete key, removes a source.
- **Stack button and tabs**: one tab for each source, with the SNR of the frame being viewed (the best source in green), and beside them the Stack button, which says the SNR of the stack. Stack on: the picture is the stack. Stack off, or a tab clicked: it is the source of that tab. Switching the button compares the stack with the source, at the same zoom and position. The keys 1 to 9 pick a source and 0 (or S) switches the stack.
- **Picture**: it starts at 100%. Zoom by typing a value, with the + and - buttons (they go through 100%) or with Ctrl and the wheel, and drag the picture to pan. Line doubling enlarges the picture without smoothing it, which is what shows the difference between sources; Smooth is the other choice. The Chroma button decodes the picture to colour; Chroma settings opens the configuration of the decoder (PAL filter, NTSC 1D, 2D or 3D, gain, phase, noise reduction). A 3D decoder looks at the frames either side, so for the stack those are stacked too. Enhance changes the brightness, contrast and gamma of what is shown, to bring out the defects, and nothing that is stacked.
- **SNR curve** under the picture: the white SNR of the selected tab over the whole disc, with a marker on the frame. For a source it is the SNR of its metadata; the stack has none, so it is measured from the stacked fields as the frames are viewed. Click to go to a frame.
- **Frame navigation**: first, previous, next and last frame, the frame number, a slider; the left and right keys step.
- **Stacking mode**: auto or one of the modes of ld-disc-stacker. In auto the status bar says which one the number of sources gives. Edit > Stacking options sets the rest: the threshold of the smart modes, differential dropout detection, pass-through, the integrity check (used when processing to a file) and the field order.
- **Status bar**: how many sources contribute to the frame, the mode, and what is shown.
- **View > Stack output**: a window with the stack, which stays in view while the main window shows a source.

The stack shown is made with the same code as ld-disc-stacker, for the frame viewed only.

### Menu

- **File > Open session** and **Export session** load and save a `.stack` file: a JSON file with the sources (file, on or off, offset), the stacking options, the output file and what the window shows. ld-disc-stacker takes it as it is (see its page), so a session made here can be stacked from the command line.
- **File > Process** asks where to save the stack and the metadata format (SQLite or JSON), then stacks the sources that are on. The video is TBC or CVBS as the extension of the file says; when that is not the format of the sources, ld-tbc-to-cvbs or ld-cvbs-to-tbc converts the stack, and ld-sqlite-to-json or ld-json-converter converts the metadata if it is not in the format asked for. The tools are those beside ld-stacker-gui. A stack that does not finish is removed.

### Not yet

Matching sources by VBI frame number (mapping) in the GUI.
