# Code that is the same in two files (annex of the inventory, branch `audit-refactor`)

Made with a search for runs of 8 lines (not blank, not a comment or a brace, spaces ignored) found in two different files of `src/` (the tests, `libs/` and the generated `deemp.h` left out). The number
is how many such runs the two files share; it is a way to find candidates, not a measure of how much is copied. Only pairs with 6 or more are listed. The pairs that matter are in the inventory
(E-04, I-03, K-01, F-06, J-05, J-01); the others (the two ld-chroma-decoder encoders, `transformpal2d` and `3d`, a `.cpp` against its own `.h`) were looked at and are the normal likeness of files that do
the same kind of job.

```
 256  src/ld-export-decode-metadata/jsonio.cpp  <->  src/library/tbc/jsonio.cpp
 182  src/ld-export-decode-metadata/exportmetadata.cpp  <->  src/library/tbc/lddecodemetadata.cpp
 142  src/efm-decoder/tools/efm-decoder-data/src/decoders/dec_data24torawsector.cpp  <->  src/ld-process-efm/Datatypes/sector.h
  70  src/ld-export-decode-metadata/jsonio.h  <->  src/library/tbc/jsonio.h
  50  src/efm-decoder/tools/efm-decoder-audio/src/readers/reader_data24section.cpp  <->  src/efm-decoder/tools/efm-decoder-data/src/readers/reader_data24section.cpp
  40  src/ld-disc-stacker/main.cpp  <->  src/ld-dropout-correct/main.cpp
  38  src/ld-process-vbi/main.cpp  <->  src/ld-process-vits/main.cpp
  37  src/ld-disc-stacker/stackingpool.cpp  <->  src/ld-dropout-correct/correctorpool.cpp
  27  src/efm-decoder/tools/efm-decoder-data/src/decoders/dec_rawsectortosector.cpp  <->  src/ld-process-efm/Datatypes/sector.h
  24  src/ld-export-decode-metadata/exportmetadata.h  <->  src/library/tbc/lddecodemetadata.h
  23  src/efm-decoder/tools/efm-decoder-audio/src/main.cpp  <->  src/efm-decoder/tools/efm-decoder-data/src/main.cpp
  21  src/efm-decoder/tools/efm-decoder-audio/src/main.cpp  <->  src/efm-decoder/tools/efm-decoder-d24/src/main.cpp
  21  src/efm-decoder/tools/efm-decoder-d24/src/main.cpp  <->  src/efm-decoder/tools/efm-decoder-data/src/main.cpp
  19  src/efm-decoder/tools/efm-decoder-d24/src/main.cpp  <->  src/efm-decoder/tools/efm-decoder-f2/src/main.cpp
  19  src/efm-decoder/tools/efm-decoder-audio/src/main.cpp  <->  src/efm-decoder/tools/efm-decoder-f2/src/main.cpp
  19  src/efm-decoder/tools/efm-decoder-data/src/main.cpp  <->  src/efm-decoder/tools/efm-decoder-f2/src/main.cpp
  18  src/ld-process-ac3/decode/main.cpp  <->  src/ld-process-ac3/demodulate/main.cpp
  16  src/efm-decoder/tools/efm-decoder-f2/src/writers/writer_f2section.h  <->  src/efm-decoder/tools/efm-stacker-f2/src/writers/writer_f2section.h
  16  src/efm-decoder/tools/efm-decoder-audio/src/decoders/decoders.h  <->  src/efm-decoder/tools/efm-decoder-data/src/decoders/decoders.h
  16  src/efm-decoder/tools/efm-decoder-data/src/decoders/decoders.h  <->  src/efm-decoder/tools/efm-decoder-f2/src/decoders/decoders.h
  16  src/efm-decoder/tools/efm-decoder-audio/src/readers/reader_data24section.h  <->  src/efm-decoder/tools/efm-decoder-data/src/readers/reader_data24section.h
  16  src/efm-decoder/tools/efm-decoder-audio/src/decoders/decoders.h  <->  src/efm-decoder/tools/efm-decoder-f2/src/decoders/decoders.h
  14  src/ld-analyse/mainwindow.cpp  <->  src/ld-stacker-gui/mainwindow.cpp
  13  src/efm-decoder/tools/efm-decoder-d24/src/decoders/decoders.h  <->  src/efm-decoder/tools/efm-decoder-f2/src/decoders/decoders.h
  13  src/efm-decoder/tools/efm-decoder-d24/src/decoders/decoders.h  <->  src/efm-decoder/tools/efm-decoder-data/src/decoders/decoders.h
  13  src/efm-decoder/tools/efm-decoder-audio/src/decoders/decoders.h  <->  src/efm-decoder/tools/efm-decoder-d24/src/decoders/decoders.h
  13  src/ld-process-vbi/decoderpool.cpp  <->  src/ld-process-vits/processingpool.cpp
  12  src/ld-chroma-decoder/encoder/ntscencoder.cpp  <->  src/ld-chroma-decoder/encoder/palencoder.cpp
  12  src/efm-decoder/tools/efm-decoder-d24/src/readers/reader_f2section.h  <->  src/efm-decoder/tools/efm-stacker-f2/src/readers/reader_f2section.h
  12  src/ld-chroma-decoder/transformpal2d.cpp  <->  src/ld-chroma-decoder/transformpal3d.cpp
  11  src/efm-decoder/tools/efm-stacker-f2/src/main.cpp  <->  src/efm-decoder/tools/vfs-verifier/src/main.cpp
  11  src/efm-decoder/tools/efm-decoder-d24/src/efm_processor.h  <->  src/efm-decoder/tools/efm-decoder-data/src/efm_processor.h
  11  src/efm-decoder/tools/efm-decoder-d24/src/efm_processor.h  <->  src/efm-decoder/tools/efm-decoder-f2/src/efm_processor.h
  11  src/efm-decoder/tools/efm-decoder-audio/src/efm_processor.h  <->  src/efm-decoder/tools/efm-decoder-d24/src/efm_processor.h
  11  src/ld-analyse/dropoutanalysisdialog.cpp  <->  src/ld-analyse/visibledropoutanalysisdialog.cpp
  11  src/efm-decoder/tools/efm-decoder-data/src/efm_processor.h  <->  src/efm-decoder/tools/efm-decoder-f2/src/efm_processor.h
  11  src/efm-decoder/tools/efm-decoder-audio/src/efm_processor.h  <->  src/efm-decoder/tools/efm-decoder-f2/src/efm_processor.h
  11  src/efm-decoder/tools/efm-decoder-audio/src/efm_processor.h  <->  src/efm-decoder/tools/efm-decoder-data/src/efm_processor.h
  10  src/ld-export-decode-metadata/exportdropouts.h  <->  src/library/tbc/dropouts.h
  10  src/ld-analyse/blacksnranalysisdialog.cpp  <->  src/ld-analyse/whitesnranalysisdialog.cpp
   9  src/ld-chroma-decoder/comb.cpp  <->  src/ld-chroma-decoder/comb.h
   9  src/efm-decoder/tools/efm-decoder-f2/src/main.cpp  <->  src/efm-decoder/tools/efm-stacker-f2/src/main.cpp
   9  src/efm-decoder/tools/efm-decoder-d24/src/main.cpp  <->  src/efm-decoder/tools/vfs-verifier/src/main.cpp
   9  src/efm-decoder/tools/efm-decoder-f2/src/main.cpp  <->  src/efm-decoder/tools/vfs-verifier/src/main.cpp
   9  src/efm-decoder/tools/efm-decoder-audio/src/main.cpp  <->  src/efm-decoder/tools/vfs-verifier/src/main.cpp
   9  src/efm-decoder/tools/efm-decoder-data/src/main.cpp  <->  src/efm-decoder/tools/vfs-verifier/src/main.cpp
   9  src/ld-export-decode-metadata/exportdropouts.cpp  <->  src/library/tbc/dropouts.cpp
   9  src/efm-decoder/tools/efm-decoder-audio/src/main.cpp  <->  src/efm-decoder/tools/efm-stacker-f2/src/main.cpp
   9  src/efm-decoder/tools/efm-decoder-data/src/main.cpp  <->  src/efm-decoder/tools/efm-stacker-f2/src/main.cpp
   9  src/efm-decoder/tools/efm-decoder-d24/src/main.cpp  <->  src/efm-decoder/tools/efm-stacker-f2/src/main.cpp
   8  src/ld-process-vbi/decoderpool.h  <->  src/ld-process-vits/processingpool.h
   8  src/ld-chroma-decoder/transformpal.cpp  <->  src/ld-chroma-decoder/transformpal3d.h
   8  src/ld-chroma-decoder/transformpal3d.cpp  <->  src/ld-chroma-decoder/transformpal3d.h
   8  src/ld-chroma-decoder/transformpal2d.h  <->  src/ld-chroma-decoder/transformpal3d.cpp
   8  src/library/tbc/stackercore.cpp  <->  src/library/tbc/stackercore.h
   8  src/ld-chroma-decoder/transformpal.cpp  <->  src/ld-chroma-decoder/transformpal.h
   8  src/ld-chroma-decoder/ntscdecoder.cpp  <->  src/ld-chroma-decoder/ntscdecoder.h
   8  src/ld-chroma-decoder/transformpal.cpp  <->  src/ld-chroma-decoder/transformpal3d.cpp
   8  src/ld-chroma-decoder/transformpal.cpp  <->  src/ld-chroma-decoder/transformpal2d.h
   8  src/ld-chroma-decoder/transformpal.h  <->  src/ld-chroma-decoder/transformpal2d.h
   8  src/ld-export-metadata/closedcaptions.cpp  <->  src/ld-export-metadata/closedcaptions.h
   8  src/ld-chroma-decoder/transformpal.h  <->  src/ld-chroma-decoder/transformpal2d.cpp
   8  src/ld-chroma-decoder/transformpal.h  <->  src/ld-chroma-decoder/transformpal3d.h
   8  src/ld-chroma-decoder/transformpal2d.h  <->  src/ld-chroma-decoder/transformpal3d.h
   8  src/ld-chroma-decoder/transformpal.h  <->  src/ld-chroma-decoder/transformpal3d.cpp
   8  src/efm-decoder/tools/efm-decoder-audio/src/main.cpp  <->  src/ld-process-efm/main.cpp
   8  src/library/tbc/videoiddecoder.cpp  <->  src/library/tbc/videoiddecoder.h
   8  src/ld-chroma-deco```
