/******************************************************************************
 * infopanel.cpp
 * ld-analyse - the numbers of the disc, shown over the picture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#include "infopanel.h"

#include <QCloseEvent>
#include <QLabel>
#include <QVBoxLayout>

#include "pintitlebar.h"

InfoPanel::InfoPanel(QWidget *parent) : QFrame(parent)
{
    setObjectName("infoPanel");
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet("QFrame#infoPanel { background: rgba(12, 12, 14, 205); border: 1px solid rgba(255, 255, 255, 40); border-radius: 6px; }"
                  "QLabel { color: #e6e6ea; font-size: 12px; background: transparent; }"
                  "QLabel#headline { color: #f2b84a; font-family: monospace; font-size: 26px; font-weight: 700; }");
    // The title bar has the pin (pressed in: the window stays when the other controls go away) and the button that closes it
    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    titleBar = new PinTitleBar(tr("Disc numbers"), this);
    titleBar->setPinToolTip(tr("Pinned: this display stays when the other controls go away, as the screen of a player does. Not pinned: it goes with them."));
    connect(titleBar, &PinTitleBar::pinToggled, this, [this](bool pinned) { emit autoHideChanged(!pinned); });
    connect(titleBar, &PinTitleBar::closeRequested, this, &QWidget::close);
    outer->addWidget(titleBar);
    QWidget *content = new QWidget(this);
    content->setAttribute(Qt::WA_StyledBackground, false);
    outer->addWidget(content);
    QVBoxLayout *layout = new QVBoxLayout(content);
    layout->setContentsMargins(14, 8, 14, 12);
    layout->setSpacing(6);
    headlineLabel = new QLabel(this);
    headlineLabel->setObjectName("headline");
    headlineLabel->setAlignment(Qt::AlignRight);
    body = new QLabel(this);
    body->setTextFormat(Qt::RichText);
    layout->addWidget(headlineLabel);
    layout->addWidget(body);
    setMinimumWidth(230);
}

void InfoPanel::setRows(const QString &headline, const Rows &rows)
{
    headlineText = headline;
    shownRows = rows;
    headlineLabel->setText(headline);
    headlineLabel->setVisible(!headline.isEmpty());
    QString html = QStringLiteral("<table cellspacing='0' cellpadding='2'>");
    for (const auto &row: rows) {
        html += QStringLiteral("<tr><td style='color:#9a9aa2'>%1&nbsp;&nbsp;</td><td align='right'>%2</td></tr>")
                    .arg(row.first.toHtmlEscaped(), row.second.toHtmlEscaped());
    }
    html += QStringLiteral("</table>");
    body->setText(html);
    adjustSize();
}

bool InfoPanel::autoHide() const
{
    return !titleBar->pinned();
}

void InfoPanel::setAutoHide(bool on)
{
    titleBar->setPinned(!on);
}

void InfoPanel::closeEvent(QCloseEvent *event)
{
    QFrame::closeEvent(event);
    emit closed();
}
