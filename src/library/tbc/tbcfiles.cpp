/******************************************************************************
 * tbcfiles.cpp
 * ld-decode-tools TBC library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "tbcfiles.h"

#include <QFileInfo>

#include "cvbs.h"
#include "ldxcapture.h"

namespace TbcFiles {

namespace {

// Whether fileName ends with suffix, whatever the case
bool endsWith(const QString &fileName, const char *suffix)
{
    return fileName.endsWith(suffix, Qt::CaseInsensitive);
}

// fileName without its last length characters
QString without(const QString &fileName, qsizetype length)
{
    return fileName.left(fileName.length() - length);
}

} // namespace

bool isOrcYcFile(const QString &fileName)
{
    return endsWith(fileName, ".tbcy") || endsWith(fileName, ".tbcc");
}

bool isChromaFile(const QString &fileName)
{
    if (LdxCapture::isLdxName(fileName)) return LdxCapture::isChromaTrack(fileName);
    return endsWith(fileName, ".tbcc") || endsWith(fileName, "_chroma.tbc");
}

bool isLumaFile(const QString &fileName)
{
    return endsWith(fileName, ".tbcy");
}

namespace {

// The extension of a name that has a mapping: .tbc or .cvbs (with its dot, as it is written), or empty
QString mappableExtension(const QString &fileName)
{
    for (const char *extension: { ".tbc", ".cvbs" }) {
        if (endsWith(fileName, extension)) return fileName.right(static_cast<qsizetype>(qstrlen(extension)));
    }
    return QString();
}

} // namespace

bool isMappedFileName(const QString &fileName)
{
    const QString extension = mappableExtension(fileName);
    return !extension.isEmpty() && without(fileName, extension.length()).endsWith(QLatin1String("_map"), Qt::CaseInsensitive);
}

QString mappedFileName(const QString &fileName)
{
    const QString extension = mappableExtension(fileName);
    // (the chroma file of a Y/C pair has none of its own: ld-discmap takes a luma or composite file)
    if (extension.isEmpty() || isMappedFileName(fileName) || isChromaFile(fileName)) return QString();
    return without(fileName, extension.length()) + QStringLiteral("_map") + extension;
}

QString originalFileName(const QString &fileName)
{
    if (!isMappedFileName(fileName)) return fileName;
    const QString extension = mappableExtension(fileName);
    return without(fileName, extension.length() + 4) + extension;
}

bool isCaptureFileName(const QString &fileName)
{
    if (LdxCapture::isLdxName(fileName)) return true;
    return endsWith(fileName, ".tbc") || endsWith(fileName, ".tbcy") || endsWith(fileName, ".tbcc") || Cvbs::isDataFileName(fileName);
}

bool isStackSourceName(const QString &fileName)
{
    if (!isCaptureFileName(fileName)) return false;
    if (LdxCapture::isLdxName(fileName)) return !LdxCapture::isChromaTrack(fileName);
    if (endsWith(fileName, ".cvbsc")) return false;
    return !isChromaFile(fileName);
}

QString baseName(const QString &fileName)
{
    if (endsWith(fileName, "_chroma.tbc")) return without(fileName, 11);
    if (endsWith(fileName, ".tbcy") || endsWith(fileName, ".tbcc")) return without(fileName, 5);
    if (endsWith(fileName, ".tbc")) return without(fileName, 4);
    return fileName;
}

QString lumaFileName(const QString &fileName)
{
    if (LdxCapture::isLdxName(fileName)) return LdxCapture::lumaOfChroma(fileName);
    if (endsWith(fileName, ".tbcc")) return without(fileName, 1) + "y";
    if (endsWith(fileName, "_chroma.tbc")) return without(fileName, 11) + ".tbc";
    return fileName;
}

QString chromaFileName(const QString &fileName)
{
    if (LdxCapture::isLdxName(fileName)) return LdxCapture::isChromaTrack(fileName) ? fileName : LdxCapture::chromaOfLuma(fileName);
    if (isChromaFile(fileName)) return fileName;

    QString partner;
    if (endsWith(fileName, ".tbcy")) partner = without(fileName, 1) + "c";
    else if (endsWith(fileName, ".tbc")) partner = without(fileName, 4) + "_chroma.tbc";

    return (!partner.isEmpty() && QFileInfo::exists(partner)) ? partner : QString();
}

QString metadataBaseName(const QString &fileName)
{
    if (isOrcYcFile(fileName)) return baseName(fileName) + ".tbc";
    return fileName;
}

} // namespace TbcFiles
