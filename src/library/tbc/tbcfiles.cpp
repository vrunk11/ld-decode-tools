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
