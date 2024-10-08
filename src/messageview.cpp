/*
 * messageview.cpp - message data for chatview
 * Copyright (C) 2010-2017  Sergey Ilinykh
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "messageview.h"

#include "common.h"
#include "psioptions.h"
#include "textutil.h"

#include <QTextDocument>

static const QString me_cmd = "/me ";

// ======================================================================
// MessageView
// ======================================================================
MessageView::MessageView(Type t) : _type(t), _status(0), _statusPriority(0), _dateTime(QDateTime::currentDateTime()) { }

MessageView MessageView::fromPlainText(const QString &text, Type type)
{
    MessageView mv(type);
    mv.setPlainText(text);
    return mv;
}

MessageView MessageView::fromHtml(const QString &text, Type type)
{
    MessageView mv(type);
    mv.setHtml(text);
    return mv;
}

MessageView MessageView::urlsMessage(const QMap<QString, QString> &urls)
{
    MessageView mv(Urls);
    mv._type = Urls;
    mv._urls = urls;
    return mv;
}

MessageView MessageView::subjectMessage(const QString &subject, const QString &prefix)
{
    MessageView mv(Subject);
    mv._text     = TextUtil::escape(prefix);
    mv._userText = subject;
    return mv;
}

MessageView MessageView::mucJoinMessage(const QString &nick, int status, const QString &message,
                                        const QString &statusText, int priority)
{
    MessageView mv = MessageView::fromPlainText(message, MUCJoin);
    mv.setNick(nick);
    mv.setStatus(status);
    mv.setStatusPriority(priority);
    mv.setUserText(statusText);
    return mv;
}

MessageView MessageView::mucPartMessage(const QString &nick, const QString &message, const QString &statusText)
{
    MessageView mv = MessageView::fromPlainText(message, MUCPart);
    mv.setNick(nick);
    mv.setUserText(statusText);
    return mv;
}

MessageView MessageView::nickChangeMessage(const QString &nick, const QString &newNick)
{
    MessageView mv = MessageView::fromPlainText(QObject::tr("%1 is now known as %2").arg(nick, newNick), NickChange);
    mv.setNick(nick);
    mv.setUserText(newNick);
    return mv;
}

MessageView MessageView::reactionsMessage(const QString &nick, const QString &targetMessageId,
                                          const QSet<QString> &reactions)
{
    MessageView mv(Reactions);
    mv.setNick(nick);
    mv.setReactionsId(targetMessageId);
    mv.setReactions(reactions);
    return mv;
}

MessageView MessageView::retractionMessage(const QString &targetMessageId)
{
    MessageView mv(MessageRetraction);
    mv.setRetractionId(targetMessageId);
    return mv;
}

MessageView MessageView::statusMessage(const QString &nick, int status, const QString &statusText, int priority)
{
    QString message = QObject::tr("%1 is now %2").arg(nick, status2txt(status));

    MessageView mv = MessageView::fromPlainText(message, Status);
    mv.setNick(nick);
    mv.setStatus(status);
    mv.setStatusPriority(priority);
    mv.setUserText(statusText);
    return mv;
}

// getters and setters

void MessageView::setPlainText(const QString &text)
{
    if (!text.isEmpty()) {
        if (_type == Message) {
            setEmote(text.startsWith(me_cmd));
        }
        _text = TextUtil::plain2rich(text);
        if (_type == Message) {
            _text = TextUtil::linkify(_text);
        }
    }
}

void MessageView::setHtml(const QString &text)
{
    if (_type == Message) {
        QString str = TextUtil::rich2plain(text).trimmed();
        setEmote(str.startsWith(me_cmd));
        if (isEmote()) {
            setPlainText(str);
            return;
        }
    }
    _text = text;
}

QString MessageView::formattedText() const
{
    QString txt = _text;

    if (isEmote() && _type == Message) {
        int cmd = txt.indexOf(me_cmd);
        txt     = txt.remove(cmd, me_cmd.length());
    }
    if (PsiOptions::instance()->getOption("options.ui.emoticons.use-emoticons").toBool())
        txt = TextUtil::emoticonify(txt);
    if (PsiOptions::instance()->getOption("options.ui.chat.legacy-formatting").toBool())
        txt = TextUtil::legacyFormat(txt);

    return txt;
}

QString MessageView::formattedUserText() const
{
    if (!_userText.isEmpty()) {
        QString text = TextUtil::plain2rich(_userText);
        text         = TextUtil::linkify(text);
        if (PsiOptions::instance()->getOption("options.ui.emoticons.use-emoticons").toBool())
            text = TextUtil::emoticonify(text);
        if (PsiOptions::instance()->getOption("options.ui.chat.legacy-formatting").toBool())
            text = TextUtil::legacyFormat(text);
        return text;
    }
    return "";
}

bool MessageView::hasStatus() const { return _type == Status || _type == MUCJoin; }
