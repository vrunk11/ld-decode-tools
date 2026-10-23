/************************************************************************

    discmapper.h

    ld-discmap - TBC and VBI alignment and correction
    Copyright (C) 2019-2022 Simon Inns

    This file is part of ld-decode-tools.

    ld-discmap is free software: you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

************************************************************************/

#ifndef DISCMAPPER_H
#define DISCMAPPER_H

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QFile>

// TBC library includes
#include "sourcevideo.h"
#include "targetvideo.h"
#include "sourceaudio.h"
#include "lddecodemetadata.h"

#include "discmap.h"

class DiscMapper
{
public:
    DiscMapper();

    // How many frames more than the capture holds between two frames a gap of the numbers may be and still be padded (see padDiscMap)
    void setGapAllowance(qint32 frames) { gapAllowance = frames; }

    bool process(QFileInfo _inputFileInfo, QFileInfo _inputMetadataFileInfo,
                 QFileInfo _outputFileInfo, bool _reverse, bool _mapOnly, bool _noStrict,
                 bool _deleteUnmappable, bool _noAudio,
                 const QString &_mapFileName = QString(), const QString &_sessionFileName = QString(), bool _saveMap = false);

private:
    QFileInfo inputFileInfo;
    QFileInfo inputMetadataFileInfo;
    QFileInfo outputFileInfo;
    bool reverse;
    bool mapOnly;
    bool noStrict;
    bool deleteUnmappable;
    bool noAudio;
    qint32 gapAllowance = 100;
    QString mapFileName;        // where the map of the capture is written as JSON (none: not written)
    QString sessionFileName;    // the stacking session the map is put in (none: not put in one)
    bool saveMap = false;       // the map is kept in the metadata of the input

    bool writeMapFile(DiscMap &discMap);
    bool writeMapToSession(DiscMap &discMap);
    bool saveMapInInput(DiscMap &discMap);
    void removeLeadInOut(DiscMap &discMap);
    void removeInvalidFramesByPhase(DiscMap &discMap);
    void correctVbiFrameNumbersUsingSequenceAnalysis(DiscMap &discMap);
    void removeDuplicateNumberedFrames(DiscMap &discMap);
    void numberPulldownFrames(DiscMap &discMap);
    bool verifyFrameNumberPresence(DiscMap &discMap);
    void reorderFrames(DiscMap &discMap);
    void padDiscMap(DiscMap &discMap);
    void rewriteFrameNumbers(DiscMap &discMap);
    void deleteUnmappableFrames(DiscMap &discMap);
    void markUnmappableFrames(DiscMap &discMap);

    bool saveDiscMap(DiscMap &discMap);
};

#endif // DISCMAPPER_H
