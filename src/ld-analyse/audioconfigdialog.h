/******************************************************************************
 * audioconfigdialog.h
 * ld-analyse - the window where the audio is set up: output, imported audio
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef AUDIOCONFIGDIALOG_H
#define AUDIOCONFIGDIALOG_H

#include <QByteArray>
#include <QColor>
#include <QDialog>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QProcess;
class QPushButton;
class QSlider;
class QSpinBox;
class QToolButton;
class QLineEdit;

// Audio config: the tab Output (sound device, colour of the waveform, the controls of the panel repeated, the panel's column
// of controls on or off) and the tab Import (a file that ffmpeg reads, cut to the length of the video, with an offset).
class AudioConfigDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AudioConfigDialog(QWidget *parent = nullptr);

    void setDevice(const QByteArray &id);
    void setColor(const QColor &colour);
    void setSidePanel(bool shown);
    void setMix(bool left, bool right, qint32 volume);
    void setOffsetMs(qint32 milliseconds);
    // The length of the video (the imported audio is cut to it) and the LDX file the audio can be saved in (empty if none)
    void setVideo(double videoSeconds, const QString &ldxFile);
    void setImportedName(const QString &name);

signals:
    void deviceChosen(const QByteArray &id);
    void colorChosen(const QColor &colour);
    void sidePanelChanged(bool shown);
    void mixChanged(bool left, bool right, qint32 volume);
    void offsetChanged(qint32 milliseconds);
    // The audio that was read, to use for this session
    void imported(const QVector<qint16> &samples, double sampleRate, const QString &name);
    void importRemoved();
    // To save the imported audio in the LDX file as a track (the 16-bit stereo samples)
    void saveRequested(const QVector<qint16> &samples, double sampleRate, const QString &name);

private:
    void browse();
    void importFile();
    void chooseColor();
    void finishedReading(int exitCode);

    QComboBox *deviceCombo;
    QPushButton *colourButton;
    QCheckBox *sideCheck;
    QToolButton *leftButton, *rightButton;
    QSlider *volumeSlider;

    QLineEdit *fileEdit;
    QSpinBox *offsetSpin;
    QPushButton *useButton, *saveButton, *removeButton;
    QLabel *status;

    QColor colour;
    double videoLength = 0.0;
    QString ldx;
    QString name;
    QProcess *ffmpeg = nullptr;
    QVector<qint16> samples;
};

#endif // AUDIOCONFIGDIALOG_H
