/*
Licensed to the Apache Software Foundation (ASF) under one
or more contributor license agreements.  See the NOTICE file
distributed with this work for additional information
regarding copyright ownership.  The ASF licenses this file
to you under the Apache License, Version 2.0 (the
"License"); you may not use this file except in compliance
with the License.  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the License is distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied.  See the License for the
specific language governing permissions and limitations
under the License.
*/

#include "qite.h"

#include <QDebug>
#include <QHoverEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextObjectInterface>

//----------------------------------//
// InteractiveTextElementController //
//----------------------------------//
InteractiveTextElementController::InteractiveTextElementController(InteractiveText *it, QObject *parent) :
    QObject(parent), itc(it)
{
    objectType = itc->registerController(this);
}

InteractiveTextElementController::~InteractiveTextElementController() { itc->unregisterController(this); }

void InteractiveTextElementController::drawObject(QPainter *painter, const QRectF &rect, QTextDocument *doc,
                                                  int posInDocument, const QTextFormat &format)
{
    Q_UNUSED(doc)
    auto elementId = InteractiveTextFormat::id(format);
    itc->markVisible(elementId);
    drawITE(painter, rect, posInDocument, format);
}

bool InteractiveTextElementController::mouseEvent(const Event &event, const QRect &rect, QTextCursor &selected)
{
    Q_UNUSED(event)
    Q_UNUSED(rect)
    Q_UNUSED(selected)
    return false;
}

void InteractiveTextElementController::hideEvent(QTextCursor &selected) { Q_UNUSED(selected) }

QCursor InteractiveTextElementController::cursor() { return QCursor(Qt::IBeamCursor); }

//---------------------------//
// InteractiveTextController //
//---------------------------//
InteractiveText::InteractiveText(QTextEdit *textEdit, int baseObjectType) :
    QObject(textEdit), _textEdit(textEdit), _baseObjectType(baseObjectType), _objectType(baseObjectType)
{
    textEdit->installEventFilter(this);
    textEdit->viewport()->installEventFilter(this);

    connect(
        textEdit->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { trackVisibility(); },
        Qt::QueuedConnection);
    connect(
        textEdit->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { trackVisibility(); },
        Qt::QueuedConnection);
    connect(textEdit, &QTextEdit::textChanged, this, &InteractiveText::trackVisibility, Qt::QueuedConnection);
}

int InteractiveText::registerController(InteractiveTextElementController *elementController)
{
    auto           objectType = _objectType++;
    QTextDocument *doc        = _textEdit->document();
    doc->documentLayout()->registerHandler(objectType, elementController);
    _controllers.insert(objectType, elementController);
    return objectType;
}

void InteractiveText::unregisterController(InteractiveTextElementController *elementController)
{
    textEdit()->document()->documentLayout()->unregisterHandler(elementController->objectType, elementController);
    _controllers.remove(elementController->objectType);
}

InteractiveTextFormat::ElementId InteractiveText::nextId() { return ++_uniqueElementId; }

void InteractiveText::insert(const InteractiveTextFormat &fmt)
{
    _textEdit->textCursor().insertText(QString(QChar::ObjectReplacementCharacter), fmt);
    // TODO check if mouse is already on the element
}

QTextCursor InteractiveText::findElement(quint32 elementId, int cursorPositionHint)
{
    QTextCursor cursor(_textEdit->document());
    cursor.setPosition(cursorPositionHint);

    cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
    QString selectedText = cursor.selectedText();
    if (selectedText.size() && selectedText[0] == QChar::ObjectReplacementCharacter) {
        QTextCharFormat fmt   = cursor.charFormat();
        auto            otype = fmt.objectType();
        if (otype >= _baseObjectType && otype < _objectType
            && fmt.property(InteractiveTextFormat::Id).toUInt() == elementId) {
            return cursor;
        }
    }

    cursor.setPosition(0);
    QString elText(QChar::ObjectReplacementCharacter);
    while (!(cursor = _textEdit->document()->find(elText, cursor)).isNull()) {
        QTextCharFormat fmt   = cursor.charFormat();
        auto            otype = fmt.objectType();
        if (otype >= _baseObjectType && otype < _objectType
            && fmt.property(InteractiveTextFormat::Id).toUInt() == elementId) {
            break;
        }
    }
    return cursor;
}

bool InteractiveText::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == _textEdit && event->type() == QEvent::Resize) {
        trackVisibility();
        return false;
    }

    bool ourEvent = (obj == _textEdit
                     && (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverMove
                         || event->type() == QEvent::HoverLeave))
        || (obj == _textEdit->viewport() && event->type() == QEvent::MouseButtonPress);
    if (!ourEvent) {
        return false;
    }

    bool   ret          = false;
    bool   leaveHandled = false;
    QPoint pos; // relative to visible part.
    if (event->type() == QEvent::MouseButtonPress) {
        pos = static_cast<QMouseEvent *>(event)->pos();
    } else {
        pos = static_cast<QHoverEvent *>(event)->pos();
    }

    if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverMove
        || event->type() == QEvent::MouseButtonPress) {
        QPoint viewportOffset(_textEdit->horizontalScrollBar()->value(), _textEdit->verticalScrollBar()->value());
        int    docLPos = _textEdit->document()->documentLayout()->hitTest(pos + viewportOffset, Qt::ExactHit);
        if (docLPos != -1) {
            QTextCursor cursor(_textEdit->document());
            cursor.setPosition(docLPos);
            cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
            const auto &selection = cursor.selectedText();
            if ((!selection.isEmpty()) && (selection[0] == QChar::ObjectReplacementCharacter)) {

                auto  format            = cursor.charFormat();
                auto  elementId         = format.property(InteractiveTextFormat::Id).toUInt();
                auto  ot                = format.objectType();
                auto *elementController = _controllers.value(ot);
                if (elementController) {
                    // we are definitely on a known interactive element.
                    // first we have to check what was before to generate proper events.
                    bool isEnter = !_lastMouseHandled || _lastElementId != elementId;
                    if (isEnter && _lastMouseHandled) { // jump from another element
                        checkAndGenerateLeaveEvent(event);
                    }
                    leaveHandled = true;

                    QRect rect = elementRect(cursor);
                    rect.translate(-viewportOffset);

                    InteractiveTextElementController::Event iteEvent;
                    iteEvent.qevent = event;

                    iteEvent.pos = QPoint(pos.x() - rect.left(), pos.y() - rect.top());
                    // qDebug() << "mouse" << pos << "event rel pos" << iteEvent.pos << rect;
                    if (event->type() == QEvent::MouseButtonPress) {
                        iteEvent.type = InteractiveTextElementController::EventType::Click;
                    } else {
                        iteEvent.type = isEnter ? InteractiveTextElementController::EventType::Enter
                                                : InteractiveTextElementController::EventType::Move;
                    }

                    ret = elementController->mouseEvent(iteEvent, rect, cursor);
                    if (ret) {
                        _lastCursorPositionHint = cursor.position();
                        _lastElementId          = elementId;
                        _textEdit->viewport()->setCursor(elementController->cursor());
                    } else {
                        _textEdit->viewport()->setCursor(Qt::IBeamCursor);
                    }
                }
            }
        }
    }

    if (!leaveHandled) {
        // not checked yet if we need leave event.This also means we are not on an element.
        checkAndGenerateLeaveEvent(event);
        if (_lastMouseHandled) {
            _textEdit->viewport()->setCursor(Qt::IBeamCursor);
        }
    }

    _lastMouseHandled = ret;
    return ret;
}

void InteractiveText::checkAndGenerateLeaveEvent(QEvent *event)
{
    if (!_lastMouseHandled) {
        return;
    }
    QTextCursor cursor = findElement(_lastElementId, _lastCursorPositionHint);
    if (!cursor.isNull()) {
        auto                              fmt        = cursor.charFormat();
        InteractiveTextElementController *controller = _controllers.value(fmt.objectType());
        if (!controller) {
            return;
        }

        InteractiveTextElementController::Event iteEvent;
        iteEvent.qevent = event;
        iteEvent.type   = InteractiveTextElementController::EventType::Leave;

        controller->mouseEvent(iteEvent, QRect(), cursor);
    }
}

void InteractiveText::markVisible(const InteractiveTextFormat::ElementId &id) { _visibleElements.insert(id); }

// returns rect of the interactive selected (from left to right) element in global coords.
// So consider coverting into viewport coordinates if needed.
QRect InteractiveText::elementRect(const QTextCursor &cursor) const
{
    QRect       ret;
    auto        block      = cursor.block();
    auto        controller = _controllers.value(cursor.charFormat().objectType());
    QTextCursor anchorCursor(cursor);
    anchorCursor.movePosition(QTextCursor::Left);
    if (controller && block.isValid() && block.isVisible()) {
        // qDebug() << "block pos" << block.position() << "lines" << block.lineCount() << "layout lines" <<
        // block.layout()->lineCount();
        auto      posInBlock = anchorCursor.position() - block.position();
        QTextLine line       = block.layout()->lineForTextPosition(posInBlock);
        if (line.isValid()) {
            // qDebug() << "  line rect" << line.rect();
            auto x = line.cursorToX(posInBlock);
            auto s = controller->intrinsicSize(_textEdit->document(), anchorCursor.position(), cursor.charFormat());
            ret    = QRect(QPoint(0, 0), s.toSize());
            ret.moveBottomLeft(QPoint(int(x), int(line.rect().bottom())));
            ret.translate(_textEdit->document()->documentLayout()->blockBoundingRect(block).topLeft().toPoint());
        }
    }
    return ret;
}

void InteractiveText::trackVisibility()
{
    // qDebug() << "check visibility";
    QMutableSetIterator<InteractiveTextFormat::ElementId> it(_visibleElements);
    QPoint viewportOffset(_textEdit->horizontalScrollBar()->value(), _textEdit->verticalScrollBar()->value());
    // auto startCursor = _textEdit->cursorForPosition(QPoint(0,0));
    QRect viewPort(QPoint(0, 0), _textEdit->viewport()->size());

    while (it.hasNext()) {
        auto id     = it.next();
        auto cursor = findElement(
            id); // FIXME this call is not optimal. but internally it uses qtextdocument, so it won't slowdoan that much
        if (!cursor.isNull()) {
            auto cr = elementRect(cursor);
            cr.translate(-viewportOffset);

            // now we can check if it's still on the screen
            if (cr.isNull() || !viewPort.intersects(cr)) {
                auto c = _controllers.value(cursor.charFormat().objectType());
                if (c) {
                    c->hideEvent(cursor);
                    it.remove();
                }
            }
        }
    }
}
