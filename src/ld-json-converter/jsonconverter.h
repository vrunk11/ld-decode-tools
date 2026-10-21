/******************************************************************************
 * jsonconverter.h
 * ld-json-converter - JSON converter tool for ld-decode
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025 Simon Inns
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef JSONCONVERTER_H
#define JSONCONVERTER_H

#include <QString>
#include "lddecodemetadata.h"

class JsonConverter
{
public:
    JsonConverter(const QString &inputJsonFilename, const QString &outputSqliteFilename);
    ~JsonConverter();

    bool process();

private:
    QString m_inputJsonFilename;
    QString m_outputSqliteFilename;

    void reportJsonContents(LdDecodeMetaData &metaData);
};

#endif // JSONCONVERTER_H