/******************************************************************************
 * metadatadialog.h
 * ldx-packer - look at the metadata of a capture, change its parameters, or replace them with those of a .json or .db file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef METADATADIALOG_H
#define METADATADIALOG_H

#include "document.h"

#include <QDialog>
#include <QJsonObject>

class QLabel;
class QPlainTextEdit;

// The metadata of a capture are ld-decode metadata as JSON (section 8 of the specification): the parameters of the video and of the audio, and a
// record for each field. The parameters are shown to be changed; the fields are kept as they are (or are those of the file that replaces them).
class MetadataDialog : public QDialog
{
    Q_OBJECT

public:
    MetadataDialog(Document &document, int index, QWidget *parent = nullptr);

    // False when the metadata could not be read: the dialog is not to be shown.
    bool ok() const { return m_ok; }
    void accept() override;

private:
    void replaceFromFile();
    void exportAll();
    void showRoot(const QJsonObject &root);

    Document &m_document;
    int m_index;
    bool m_ok = false;
    bool m_replaced = false;
    QJsonObject m_root;
    QString m_shown;
    QLabel *m_summary = nullptr;
    QPlainTextEdit *m_text = nullptr;
};

#endif // METADATADIALOG_H
