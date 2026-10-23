/************************************************************************

    frame.cpp

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

#include "frame.h"

#include <cmath>

Frame::Frame(const qint32 seqFrameNumber, const qint32 vbiFrameNumber, const bool isPictureStop,
             const bool isPullDown, const bool isLeadInOrOut, const bool isMarkedForDeletion,
             const double frameQuality, const bool isPadded, const bool isClvOffset,
             const qint32 firstField, const qint32 secondField,
             const qint32 firstFieldPhase, const qint32 secondFieldPhase)
           : m_seqFrameNumber(seqFrameNumber),  m_vbiFrameNumber(vbiFrameNumber), m_isPictureStop(isPictureStop),
             m_isPullDown(isPullDown), m_isLeadInOrOut(isLeadInOrOut), m_isMarkedForDeletion(isMarkedForDeletion),
             m_frameQuality(frameQuality), m_isPadded(isPadded), m_isClvOffset(isClvOffset),
             m_firstField(firstField), m_secondField(secondField),
             m_firstFieldPhase(firstFieldPhase), m_secondFieldPhase(secondFieldPhase)
{
}

// Custom streaming operator (for debug)
QDebug operator<<(QDebug dbg, const Frame &frame)
{
    // Output the object's contents to debug:
    dbg.nospace().noquote() << "Frame(" <<
                               "seqFrameNumber " << frame.seqFrameNumber() <<
                               ", vbiFrameNumber " << frame.vbiFrameNumber() <<
                               ", isPictureStop " << frame.isPictureStop() <<
                               ", isLeadInOrOut " << frame.isLeadInOrOut() <<
                               ", isMarkedForDeletion " << frame.isMarkedForDeletion() <<
                               ", frameQuality " << frame.frameQuality() <<
                               ", isPadded " << frame.isPadded() <<
                               ", isClvOffset " << frame.isClvOffset() <<
                               ", firstField " << frame.firstField() <<
                               ", secondField" << frame.secondField() <<
                               ", firstFieldPhase " << frame.firstFieldPhase() <<
                               ", secondFieldPhase " << frame.secondFieldPhase() <<
                               ")";

    return dbg.maybeSpace();
}

// Get methods
qint32 Frame::seqFrameNumber() const
{
    return m_seqFrameNumber;
}

qint32 Frame::vbiFrameNumber() const
{
    return m_vbiFrameNumber;
}

bool Frame::isPictureStop() const
{
    return m_isPictureStop;
}

bool Frame::isPullDown() const
{
    return m_isPullDown;
}

bool Frame::isLeadInOrOut() const
{
    return m_isLeadInOrOut;
}

bool Frame::isUnmappable() const
{
    return m_isUnmappable;
}

void Frame::isUnmappable(bool value)
{
    m_isUnmappable = value;
}

bool Frame::isNumberCorrected() const
{
    return m_isNumberCorrected;
}

void Frame::isNumberCorrected(bool value)
{
    m_isNumberCorrected = value;
}

bool Frame::isNumberUnconfirmed() const
{
    return m_isNumberUnconfirmed;
}

void Frame::isNumberUnconfirmed(bool value)
{
    m_isNumberUnconfirmed = value;
}

bool Frame::isMarkedForDeletion() const
{
    return m_isMarkedForDeletion;
}

double Frame::frameQuality() const
{
    return m_frameQuality;
}

bool Frame::isPadded() const
{
    return m_isPadded;
}

bool Frame::isClvOffset() const
{
    return m_isClvOffset;
}

qint32 Frame::firstField() const
{
    return m_firstField;
}

qint32 Frame::secondField() const
{
    return m_secondField;
}

qint32 Frame::firstFieldPhase() const
{
    return m_firstFieldPhase;
}

qint32 Frame::secondFieldPhase() const
{
    return m_secondFieldPhase;
}


// Set methods
void Frame::seqFrameNumber(qint32 value)
{
    m_seqFrameNumber = value;
}

void Frame::vbiFrameNumber(qint32 value)
{
    m_vbiFrameNumber = value;
}

void Frame::isPictureStop(bool value)
{
    m_isPictureStop = value;
}

void Frame::isPullDown(bool value)
{
    m_isPullDown = value;
}

void Frame::isLeadInOrOut(bool value)
{
    m_isLeadInOrOut = value;
}

void Frame::isMarkedForDeletion(bool value)
{
    m_isMarkedForDeletion = value;
}

void Frame::frameQuality(double value)
{
    m_frameQuality = value;
}

void Frame::isPadded(bool value)
{
    m_isPadded = value;
}

void Frame::isClvOffset(bool value)
{
    m_isClvOffset = value;
}

void Frame::firstField(qint32 value)
{
    m_firstField = value;
}

void Frame::secondField(qint32 value)
{
    m_secondField = value;
}

void Frame::firstFieldPhase(qint32 value)
{
    m_firstFieldPhase = value;
}

void Frame::secondFieldPhase(qint32 value)
{
    m_secondFieldPhase = value;
}

// Override less than operator for sorting
// (a strict order: the numbers, then a pulldown frame after the frames of its number that are not pulldown. It used to be true both ways for two pulldown
// frames of the same number, which is not an order, and sorting such a map could crash)
bool Frame::operator<(const Frame& other) const
{
    if (m_vbiFrameNumber != other.m_vbiFrameNumber) return m_vbiFrameNumber < other.m_vbiFrameNumber;
    return !m_isPullDown && other.m_isPullDown;
}

double frameQualityPercent(double blackPsnr, double penaltyPercent, double syncConfPercent, qint64 dropOutLength, qint64 samplesInFrame)
{
    // Decibels to an amplitude ratio, then to a share of the reference (43 dB is the most that is expected)
    double bsnrPercent = 100.0 * std::pow(10.0, (blackPsnr - 43.0) / 20.0);
    if (bsnrPercent > 100.0) bsnrPercent = 100.0;

    // The share of the samples of the frame that are not in a dropout
    double frameDoPercent = 100.0;
    if (samplesInFrame > 0) frameDoPercent = 100.0 - 100.0 * static_cast<double>(dropOutLength) / static_cast<double>(samplesInFrame);
    if (frameDoPercent < 0.0) frameDoPercent = 0.0;

    return (bsnrPercent + penaltyPercent + syncConfPercent + (frameDoPercent * 1000.0)) / 1004.0;
}
