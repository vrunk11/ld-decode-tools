/******************************************************************************
 * externalrenderer.h
 * ld-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef EXTERNALRENDERER_H
#define EXTERNALRENDERER_H

#include <QByteArray>
#include <QImage>
#include <QMap>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QWidget>
#include <QTimer>

// Another program shows the picture: the command that the person has written is started, and the picture that is on show (as decoded, before ld-analyse
// scales or filters it) is sent to it as raw video, 8-bit I420 (YUV 4:2:0), on its standard input, at the rate of the video. The program does the scaling and the
// deinterlacing it has been set up to do (a VLC with its own filters, mpv, ffplay...), in a window of its own.
//
// The command is a command line: {width}, {height} and {fps} stand for the size of the picture and the rate of the video. For VLC:
//   vlc - --demux=rawvideo --rawvid-width={width} --rawvid-height={height} --rawvid-fps={fps} --rawvid-chroma=I420 --no-audio
// and to have it draw in the window of ld-analyse: -I dummy and --drawable-hwnd={winid} added (its own interface would have a window of its own)
//
// A program that plays video wants a stream, and a picture that stays (a still, a paused video) is a stream too: the last picture is sent again at the
// rate of the video for as long as it is on show. The program is started for a size of picture: when the view changes it to another size (or when a value that
// the command uses changes, see setVariable) it is started again, with the options that go with the new view.
class ExternalRenderer : public QObject
{
    Q_OBJECT
public:
    explicit ExternalRenderer(QObject *parent = nullptr);
    ~ExternalRenderer() override;

    // The command line that is proposed (a VLC that is installed where the system puts it)
    static QString defaultCommand();

    void setCommand(const QString &command);
    const QString &command() const { return commandLine; }

    // A widget of the window for the program to draw in, when the command says {winid} (the identifier of its native window, for VLC's --drawable-hwnd on
    // Windows, --drawable-xid on X11): the picture is then shown in the window of ld-analyse. Without {winid} the program has a window of its own.
    void setHost(QWidget *widget) { host = widget; }
    bool embeds() const { return commandLine.contains("{winid}") && host; }

    // Values that stand for a name in braces in the command ({aspect}, {deinterlace}): what a change of the view or of the aspect makes of the options of the program. When
    // one that the command uses changes the program is started again with the next picture.
    void setVariable(const QString &name, const QString &value);

    // On: the program is started with the next picture. Off: it is closed.
    void setEnabled(bool enabled);
    bool isEnabled() const { return enabledNow; }
    bool isRunning() const { return process && process->state() != QProcess::NotRunning; }

    // The picture that is on show, and the rate of the video
    void setImage(const QImage &image, double framesPerSecond);

signals:
    // The program could not be started, or it ended (its window was closed): the renderer is off then
    void stopped(const QString &message);

private:
    void start(const QImage &image, double framesPerSecond);
    void stop();
    void send();

    QString commandLine;
    QWidget *host = nullptr;
    QMap<QString, QString> variables;
    bool enabledNow = false;
    QProcess *process = nullptr;
    QTimer timer;
    QByteArray latest;              // (the picture as I420, the size the program was started for)
    qint32 pictureWidth = 0, pictureHeight = 0;     // the size of the picture that the program was started for
    qint32 width = 0, height = 0;                   // the size of the frames that are sent: those, made even (the chroma of I420 is of half the size)
    double rate = 25.0;
    qint64 dropped = 0;
};

#endif // EXTERNALRENDERER_H
