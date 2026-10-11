/******************************************************************************
 * cvbsaudio.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "cvbsaudio.h"

#include <QByteArray>
#include <QDebug>
#include <QFile>
#include <QVector>
#include <algorithm>

namespace CvbsAudio {

namespace {

// Stereo samples handled at a time
constexpr qint64 CHUNK_SAMPLES = 1 << 16;

void putU16(QByteArray &data, quint16 value)
{
    data.append(static_cast<char>(value & 0xff));
    data.append(static_cast<char>(value >> 8));
}

void putU32(QByteArray &data, quint32 value)
{
    putU16(data, static_cast<quint16>(value & 0xffff));
    putU16(data, static_cast<quint16>(value >> 16));
}

quint16 getU16(const uchar *p) { return static_cast<quint16>(p[0] | (p[1] << 8)); }
quint32 getU32(const uchar *p) { return static_cast<quint32>(getU16(p)) | (static_cast<quint32>(getU16(p + 2)) << 16); }

// The 44-byte header of a PCM WAV of dataBytes bytes of 24-bit stereo
QByteArray wavHeader(quint32 sampleRate, quint32 dataBytes)
{
    QByteArray header;
    header.append("RIFF");
    putU32(header, 36 + dataBytes);
    header.append("WAVEfmt ");
    putU32(header, 16);
    putU16(header, 1);                  // PCM
    putU16(header, 2);                  // channels
    putU32(header, sampleRate);
    putU32(header, sampleRate * 6);     // bytes per second
    putU16(header, 6);                  // bytes per sample pair
    putU16(header, 24);                 // bits
    header.append("data");
    putU32(header, dataBytes);
    return header;
}

// Find the fmt and data chunks of a WAV; dataOffset and dataBytes locate the samples
bool parseWav(QFile &file, WavInfo &info, qint64 &dataOffset)
{
    QByteArray riff = file.read(12);
    if (riff.size() != 12 || !riff.startsWith("RIFF") || riff.mid(8, 4) != "WAVE") {
        qCritical().noquote() << file.fileName() << "is not a WAV file";
        return false;
    }

    bool haveFormat = false;
    while (true) {
        const QByteArray chunkHeader = file.read(8);
        if (chunkHeader.size() != 8) break;
        const quint32 chunkSize = getU32(reinterpret_cast<const uchar *>(chunkHeader.constData()) + 4);
        const QByteArray chunkId = chunkHeader.left(4);

        if (chunkId == "fmt ") {
            const QByteArray format = file.read(chunkSize);
            if (format.size() < 16) break;
            const uchar *f = reinterpret_cast<const uchar *>(format.constData());
            const quint16 tag = getU16(f);
            const quint16 channels = getU16(f + 2);
            info.sampleRate = getU32(f + 4);
            info.bits = getU16(f + 14);
            // Plain PCM, or the extensible form with a PCM sub-format
            if ((tag != 1 && tag != 0xfffe) || channels != 2 || (info.bits != 16 && info.bits != 24)) {
                qCritical().noquote() << file.fileName() << "is not 16 or 24-bit stereo PCM (format" << tag << ","
                                      << channels << "channels," << info.bits << "bits)";
                return false;
            }
            haveFormat = true;
        } else if (chunkId == "data") {
            if (!haveFormat) break;
            dataOffset = file.pos();
            // A data size that is too big (a file still being written) is cut to the file
            const qint64 available = file.size() - dataOffset;
            const qint64 dataBytes = std::min<qint64>(chunkSize, available);
            info.numberOfSamples = dataBytes / (info.bits / 8 * 2);
            return true;
        } else {
            if (!file.seek(file.pos() + chunkSize + (chunkSize & 1))) break;
            continue;
        }
        // Chunks are padded to an even size
        if (chunkSize & 1) file.seek(file.pos() + 1);
    }

    qCritical().noquote() << "Cannot find the audio in" << file.fileName();
    return false;
}

} // namespace

QString wavFileName(const QString &baseName, qint32 pair)
{
    return QString("%1_audio_%2.wav").arg(baseName).arg(pair);
}

qint64 pcmToWav(const QString &pcmFileName, qint64 firstSample, qint64 numberOfSamples,
                const QString &wavFileName, quint32 sampleRate)
{
    QFile pcm(pcmFileName);
    if (!pcm.open(QIODevice::ReadOnly)) {
        qCritical().noquote() << "Cannot open the audio file" << pcmFileName;
        return -1;
    }
    const qint64 available = std::max<qint64>(pcm.size() / 4 - firstSample, 0);
    const qint64 samples = std::min(numberOfSamples, available);
    if (samples * 6 > 0xffffffffLL - 36) {
        qCritical().noquote() << "The audio is too long for a WAV file";
        return -1;
    }

    QFile wav(wavFileName);
    if (!wav.open(QIODevice::WriteOnly)) {
        qCritical().noquote() << "Cannot create the audio file" << wavFileName;
        return -1;
    }
    wav.write(wavHeader(sampleRate, static_cast<quint32>(samples * 6)));

    if (!pcm.seek(firstSample * 4)) return -1;
    QVector<qint16> input(CHUNK_SAMPLES * 2);
    QByteArray output;
    for (qint64 done = 0; done < samples;) {
        const qint64 count = std::min(CHUNK_SAMPLES, samples - done);
        if (pcm.read(reinterpret_cast<char *>(input.data()), count * 4) != count * 4) {
            qCritical().noquote() << "Cannot read the audio file" << pcmFileName;
            return -1;
        }

        // 16 bits become the top of 24 bits
        output.resize(static_cast<qint32>(count * 6));
        uchar *out = reinterpret_cast<uchar *>(output.data());
        for (qint64 i = 0; i < count * 2; i++) {
            const quint16 value = static_cast<quint16>(input[static_cast<qint32>(i)]);
            *out++ = 0;
            *out++ = static_cast<uchar>(value & 0xff);
            *out++ = static_cast<uchar>(value >> 8);
        }
        if (wav.write(output) != output.size()) {
            qCritical().noquote() << "Cannot write the audio file" << wavFileName;
            return -1;
        }
        done += count;
    }
    return samples;
}

bool readWavInfo(const QString &wavFileName, WavInfo &info)
{
    QFile file(wavFileName);
    if (!file.open(QIODevice::ReadOnly)) {
        qCritical().noquote() << "Cannot open the audio file" << wavFileName;
        return false;
    }
    qint64 dataOffset;
    return parseWav(file, info, dataOffset);
}

qint64 wavToPcm(const QString &wavFileName, const QString &pcmFileName)
{
    QFile wav(wavFileName);
    if (!wav.open(QIODevice::ReadOnly)) {
        qCritical().noquote() << "Cannot open the audio file" << wavFileName;
        return -1;
    }
    WavInfo info;
    qint64 dataOffset;
    if (!parseWav(wav, info, dataOffset) || !wav.seek(dataOffset)) return -1;

    QFile pcm(pcmFileName);
    if (!pcm.open(QIODevice::WriteOnly)) {
        qCritical().noquote() << "Cannot create the audio file" << pcmFileName;
        return -1;
    }

    const qint32 bytesPerSample = info.bits / 8;
    QByteArray input;
    QVector<qint16> output(CHUNK_SAMPLES * 2);
    for (qint64 done = 0; done < info.numberOfSamples;) {
        const qint64 count = std::min(CHUNK_SAMPLES, info.numberOfSamples - done);
        input = wav.read(count * 2 * bytesPerSample);
        if (input.size() != count * 2 * bytesPerSample) {
            qCritical().noquote() << "Cannot read the audio file" << wavFileName;
            return -1;
        }

        const uchar *in = reinterpret_cast<const uchar *>(input.constData());
        for (qint64 i = 0; i < count * 2; i++) {
            if (bytesPerSample == 2) {
                output[static_cast<qint32>(i)] = static_cast<qint16>(getU16(in));
                in += 2;
            } else {
                // The top 16 of 24 bits, rounded to nearest
                const qint32 value = static_cast<qint32>(static_cast<qint8>(in[2])) * 65536 + in[1] * 256 + in[0];
                output[static_cast<qint32>(i)] = static_cast<qint16>(std::clamp((value + 128) >> 8, -32768, 32767));
                in += 3;
            }
        }
        if (pcm.write(reinterpret_cast<const char *>(output.constData()), count * 4) != count * 4) {
            qCritical().noquote() << "Cannot write the audio file" << pcmFileName;
            return -1;
        }
        done += count;
    }
    return info.numberOfSamples;
}

bool WavReader::open(const QString &wavFileName)
{
    file.setFileName(wavFileName);
    if (!file.open(QIODevice::ReadOnly)) {
        qCritical().noquote() << "Cannot open the audio file" << wavFileName;
        return false;
    }
    return parseWav(file, wavInfo, dataOffset);
}

QByteArray WavReader::read(qint64 start, qint64 count)
{
    const qint32 bytesPerSample = wavInfo.bits / 8 * 2;
    const qint64 available = std::clamp<qint64>(wavInfo.numberOfSamples - start, 0, count);

    QByteArray raw;
    if (available > 0 && file.seek(dataOffset + start * bytesPerSample)) raw = file.read(available * bytesPerSample);
    const qint64 got = raw.size() / bytesPerSample;

    // 24-bit samples are used as they are, 16-bit ones widened; the rest is silence
    QByteArray samples(static_cast<qint32>(count * 6), '\0');
    if (wavInfo.bits == 24) {
        std::copy(raw.constBegin(), raw.constBegin() + got * 6, samples.begin());
    } else {
        for (qint64 i = 0; i < got * 2; i++) {
            samples[static_cast<qint32>(i * 3 + 1)] = raw[static_cast<qint32>(i * 2)];
            samples[static_cast<qint32>(i * 3 + 2)] = raw[static_cast<qint32>(i * 2 + 1)];
        }
    }
    return samples;
}

WavWriter::~WavWriter()
{
    if (file.isOpen()) close();
}

bool WavWriter::open(const QString &wavFileName, quint32 _sampleRate)
{
    sampleRate = _sampleRate;
    samplesWritten = 0;
    file.setFileName(wavFileName);
    if (!file.open(QIODevice::WriteOnly)) {
        qCritical().noquote() << "Cannot create the audio file" << wavFileName;
        return false;
    }
    // The sizes are filled in by close()
    return file.write(wavHeader(sampleRate, 0)) == 44;
}

bool WavWriter::write(const QByteArray &samples)
{
    if (file.write(samples) != samples.size()) return false;
    samplesWritten += samples.size() / 6;
    return true;
}

bool WavWriter::close()
{
    if (!file.isOpen()) return true;
    if (samplesWritten * 6 > 0xffffffffLL - 36) {
        qCritical().noquote() << "The audio is too long for a WAV file";
        file.close();
        return false;
    }
    const bool ok = file.seek(0) && file.write(wavHeader(sampleRate, static_cast<quint32>(samplesWritten * 6))) == 44;
    file.close();
    return ok;
}

} // namespace CvbsAudio
