/******************************************************************************
 * tbcfiles.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef TBCFILES_H
#define TBCFILES_H

#include <QString>

// The file names of a TBC capture, which comes in two ways of naming the two files of a
// Y/C capture (luma and chroma):
//
//   ld-decode, vhs-decode:   <name>.tbc          and  <name>_chroma.tbc
//   decode-orc:              <name>.tbcy         and  <name>.tbcc
//
// Either way the metadata is that of <name>.tbc (<name>.tbc.db or <name>.tbc.json), and
// a lone <name>.tbc is a composite capture.
namespace TbcFiles {

// <name>.tbcy or <name>.tbcc, the decode-orc naming of a Y/C pair
bool isOrcYcFile(const QString &fileName);
// A chroma file in either naming: <name>.tbcc or <name>_chroma.tbc
bool isChromaFile(const QString &fileName);
// A luma file of decode-orc naming: <name>.tbcy
bool isLumaFile(const QString &fileName);

// <name> for <name>.tbc, <name>.tbcy, <name>.tbcc and <name>_chroma.tbc; the file name
// as it is for any other
QString baseName(const QString &fileName);

// The luma (or composite) file of a chroma file: <name>.tbcy for <name>.tbcc, <name>.tbc
// for <name>_chroma.tbc. Any other file name is returned as it is.
QString lumaFileName(const QString &fileName);

// The chroma file of a capture, given any of its files, if it exists: <name>.tbcc beside
// <name>.tbcy, <name>_chroma.tbc beside <name>.tbc, or the file itself if it is one.
// Empty for a composite capture.
QString chromaFileName(const QString &fileName);

// The file that names the metadata: <name>.tbc for the files of a Y/C pair of decode-orc
// naming, and the file name as it is for any other
QString metadataBaseName(const QString &fileName);

// Whether a file that is dropped on a program is a capture it can open: a TBC file (<name>.tbc, .tbcy, .tbcc, _chroma.tbc), a CVBS file (.cvbs, .cvbsy,
// .cvbsc) or an LDX file (with or without the track after it: capture.ldx#3), whatever the case of the name
bool isCaptureFileName(const QString &fileName);

// Whether it is a capture that can be a source of a stack: a composite or luma capture of those, not the chroma file of a pair (which goes with its luma)
bool isStackSourceName(const QString &fileName);

// The file of the mapping of a capture (ld-discmap) that is kept beside it: <name>_map.tbc for <name>.tbc, <name>_map.cvbs for <name>.cvbs, with whatever
// case the extension has. Empty for any other name, and for a name that is one of a mapping already.
QString mappedFileName(const QString &fileName);
// The file that a mapping is of: <name>.tbc for <name>_map.tbc. The file name as it is if it is not one.
QString originalFileName(const QString &fileName);
// Whether the name is that of a mapping
bool isMappedFileName(const QString &fileName);

} // namespace TbcFiles

#endif // TBCFILES_H
