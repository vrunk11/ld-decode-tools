/******************************************************************************
 * mappedmetadata.h
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef MAPPEDMETADATA_H
#define MAPPEDMETADATA_H

#include "framemap.h"
#include "fieldaudio.h"
#include "lddecodemetadata.h"

#include <memory>

// The metadata of a capture seen through its map (see FrameMap): what ld-discmap writes beside the mapped copy of a capture, and what a reader that
// does not make the copy gives. The fields are those of the capture in the order of the map, with
// - isMapped set,
// - the VBI of the first field of each frame written again with the number of the map (the picture number of a CAV disc, the time code of a CLV one),
// - the dropouts of a frame that is unusable replaced by a dropout over every line,
// - a padded frame as two empty fields with pad set, the VBI of its number, and paddedAudioSamples of audio.
namespace MappedMetadata {

// The VBI words that carry a number of the disc: the hex picture number of a CAV disc, and the picture number and the time of a CLV one
qint32 frameToVbi(qint32 discFrame);
qint32 frameToClvPicNo(LdDecodeMetaData &metadata, qint32 discFrame);
qint32 frameToClvTimeCode(LdDecodeMetaData &metadata, qint32 discFrame);

// The audio samples of a padded field: a field's worth of the analogue audio of the capture (44.1 kHz at the rate of the video system; a CVBS capture
// has the rate of its WAV file). metadataFileName is the metadata of the capture, which says where its audio is for a CVBS one.
qint32 paddedAudioSamples(LdDecodeMetaData &capture, const QString &metadataFileName);

// For each field of the view (from 1, in the order the mapped copy has them), the field of the capture it is, or 0 for a blank field of a padded frame. Several
// readers of one capture (one for each thread) share the table. Returns null, with the reason in error, if the map asks for a frame the capture has no two fields for.
std::shared_ptr<const QVector<qint32>> viewFields(LdDecodeMetaData &capture, const FrameMap &map, QString &error);

// The metadata of a capture that keeps a map (LdDecodeMetaData::hasDiscMap): they are changed into those of the capture seen through the map, which every tool
// that reads the video of a capture does, so that the capture is read in the order of the disc whatever tool reads it. Returns the table of the fields of the view
// (for SourceVideo::openMappedFields), or null with no error when the metadata keep no map (nothing is changed). With an error, nothing is changed. The map
// is not applied again to the metadata that come out (they are those of a mapped capture, and keep no map).
// audioRanges, if given, is filled with the analogue audio of each field of the view (see FieldAudio), worked out from the metadata of the capture.
// captureFrames, if given, is filled with the frame of the capture that each frame of the view is (0 for a padded frame), for the alignment of the streams.
std::shared_ptr<const QVector<qint32>> applyEmbeddedMap(LdDecodeMetaData &metadata, const QString &metadataFileName, const QString &videoFileName,
                                                       QString &error, bool measuredOnly = false,
                                                       QVector<FieldAudio::Range> *audioRanges = nullptr,
                                                       QVector<qint32> *captureFrames = nullptr);

// Build the metadata of the view. capture is read with the field order the map says (its isFirstFieldFirst is set). Returns false, with the reason in
// error, if the map asks for a frame the capture does not have.
// measuredOnly: the frames whose number was not read from their own VBI (unconfirmed, replaced by the sequence, guessed: the bases u, s and g) are left out,
// as the frames that could not be numbered are (a dropout over every line).
bool build(LdDecodeMetaData &capture, const FrameMap &map, qint32 paddedAudioSamples, LdDecodeMetaData &target, QString &error,
           bool measuredOnly = false);

}

#endif // MAPPEDMETADATA_H
