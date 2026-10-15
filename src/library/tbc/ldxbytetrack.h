/******************************************************************************
 * ldxbytetrack.h
 * ld-decode-tools TBC library - the EFM or AC3 data of an LDX file as a stream of bytes
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef LDXBYTETRACK_H
#define LDXBYTETRACK_H

#include <QByteArray>
#include <QIODevice>
#include <QString>
#include <memory>

// The bytes of an `efm` (EFM t-values) or `ac3` track of an LDX file, put end to end: what the .efm or .ac3 file of the
// capture was. The tools that read those files read this device in their place, so that they take an .ldx file where they
// take a file (capture.ldx, or capture.ldx#4 for the track 4).
//
// openFile reads the file with its index (only the blocks of the track are read, and the size is known); openStdin
// reads an LDX stream on the standard input in order, the first bytes of which the caller has read already (to tell it
// from the raw data a tool takes on the standard input: see isLdxMagic).
class LdxByteTrack : public QIODevice
{
public:
    enum class Kind { Efm, Ac3 };

    explicit LdxByteTrack(QObject *parent = nullptr);
    ~LdxByteTrack() override;

    // Whether bytes are the beginning of a Matroska file (the EBML header)
    static bool isLdxMagic(const QByteArray &bytes);

    bool openFile(const QString &name, Kind kind, QString &error);
    bool openStdin(const QByteArray &alreadyRead, Kind kind, QString &error);

    bool isSequential() const override;
    qint64 size() const override;
    qint64 bytesAvailable() const override;
    void close() override;

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    struct Private;
    std::unique_ptr<Private> d;
};

#endif // LDXBYTETRACK_H
