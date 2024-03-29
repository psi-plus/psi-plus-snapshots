/*
 * invitedialog.cpp - plugin
 * Copyright (C) 2010  Evgeny Khryukin, liuch
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

#include "invatedialog.h"

using namespace GomokuGame;

InvateDialog::InvateDialog(const int account, const QString &jid, const QStringList &resources, QWidget *parent) :
    QDialog(parent), ui(new Ui::InvateDialog), accepted(false), myAcc(account), jid_(jid)
{
    setAttribute(Qt::WA_DeleteOnClose);
    ui->setupUi(this);
    ui->leJid->setText(jid_);
    ui->cbResource->addItems(resources);
    adjustSize();
}

InvateDialog::~InvateDialog() { delete ui; }

void InvateDialog::acceptBlack()
{
    emit acceptGame(myAcc, jid_ + "/" + ui->cbResource->currentText(), "black");
    accepted = true;
    accept();
    close();
}

void InvateDialog::acceptWhite()
{
    emit acceptGame(myAcc, jid_ + "/" + ui->cbResource->currentText(), "white");
    accepted = true;
    accept();
    close();
}

void InvateDialog::closeEvent(QCloseEvent *event)
{
    if (!accepted) {
        reject();
        emit rejectGame(myAcc, jid_);
    }
    event->accept();
}

// ----------------------------------------

InvitationDialog::InvitationDialog(const int account, const QString &jid, QString color, const QString &id,
                                   QWidget *parent) :
    QDialog(parent),
    accepted_(false), account_(account), id_(id)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);
    ui_.setupUi(this);

    if (color == "white")
        color = tr("white");
    else
        color = tr("black");

    ui_.lbl_text->setText(tr("Player %1 invites you \nto play gomoku. He wants to play %2.").arg(jid, color));

    connect(ui_.pb_accept, &QPushButton::clicked, this, &InvitationDialog::buttonPressed);
    connect(ui_.pb_reject, &QPushButton::clicked, this, &InvitationDialog::close);

    adjustSize();
    setFixedSize(size());
}

void InvitationDialog::buttonPressed()
{
    emit accepted(account_, id_);
    accepted_ = true;
    close();
}

void InvitationDialog::closeEvent(QCloseEvent *e)
{
    if (!accepted_)
        emit rejected(account_, id_);
    e->accept();
    close();
}
