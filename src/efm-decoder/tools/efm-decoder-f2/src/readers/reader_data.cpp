/************************************************************************

    reader_data.cpp

    efm-decoder-f2 - EFM T-values to F2 Section decoder
    Copyright (C) 2025 Simon Inns

    This file is part of ld-decode-tools.

    This application is free software: you can redistribute it and/or
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

#include "reader_data.h"
#include "tbc/logging.h"
#include "ldxcapture.h"

#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

ReaderData::ReaderData() : m_stdinStream(nullptr), m_usingStdin(false) { }

ReaderData::~ReaderData()
{
    close();
}

bool ReaderData::open(const QString &filename)
{
    if (LdxCapture::isLdxName(filename)) {
        // The EFM of an LDX file
        m_usingStdin = false;
        QString error;
        if (!m_ldx.openFile(filename, LdxByteTrack::Kind::Efm, error)) {
            qCritical().noquote() << "ReaderData::open() -" << error;
            return false;
        }
        m_device = &m_ldx;
        return true;
    }
    if (filename == "-") {
        // Use stdin: raw t-values, or an LDX stream that holds them (it begins as every Matroska file does)
        m_usingStdin = true;
        char head[4];
#ifdef Q_OS_WIN
        const int got = _read(_fileno(stdin), head, 4);
#else
        const int got = static_cast<int>(::read(fileno(stdin), head, 4));
#endif
        m_prefix = QByteArray(head, got > 0 ? got : 0);
        if (LdxByteTrack::isLdxMagic(m_prefix)) {
            QString error;
            if (!m_ldx.openStdin(m_prefix, LdxByteTrack::Kind::Efm, error)) {
                qCritical().noquote() << "ReaderData::open() -" << error;
                return false;
            }
            m_device = &m_ldx;
            m_prefix.clear();
            return true;
        }
        if (!m_file.open(stdin, QIODevice::ReadOnly)) {

            qCritical() << "ReaderData::open() - Could not open stdin for reading";
            return false;
        }
        m_device = &m_file;
        tbcDebugStream() << "ReaderData::open() - Opened stdin for data reading";
        return true;
    } else {
        // Use regular file
        m_usingStdin = false;
        m_file.setFileName(filename);
        if (!m_file.open(QIODevice::ReadOnly)) {
            qCritical() << "ReaderData::open() - Could not open file" << filename << "for reading";
            return false;
        }
        m_device = &m_file;
        tbcDebugStream() << "ReaderData::open() - Opened file" << filename << "for data reading with size"
                 << m_file.size() << "bytes";
        return true;
    }
}

QByteArray ReaderData::read(uint32_t chunkSize)
{
    if (!m_device || !m_device->isOpen()) {
        qCritical() << "ReaderData::read() - File is not open for reading";
        return QByteArray();
    }
    // (the bytes of the standard input that were read to see what it is come first)
    QByteArray data;
    if (!m_prefix.isEmpty()) {
        data = m_prefix.left(static_cast<qsizetype>(chunkSize));
        m_prefix.remove(0, data.size());
    }
    if (data.size() < static_cast<qsizetype>(chunkSize)) data += m_device->read(chunkSize - data.size());
    return data;
}

void ReaderData::close()
{
    if (m_ldx.isOpen()) m_ldx.close();
    if (!m_file.isOpen()) {
        return;
    }

    if (m_usingStdin) {
        tbcDebugStream() << "ReaderData::close(): Closed stdin";
    } else {
        tbcDebugStream() << "ReaderData::close(): Closed the data file" << m_file.fileName();
    }
    m_file.close();
    
    if (m_stdinStream) {
        delete m_stdinStream;
        m_stdinStream = nullptr;
    }
    m_usingStdin = false;
}

qint64 ReaderData::size() const
{
    if (m_usingStdin) {
        // Cannot determine size of stdin
        return -1;
    }
    return m_device ? m_device->size() : m_file.size();
}

bool ReaderData::isStdin() const
{
    return m_usingStdin;
}
