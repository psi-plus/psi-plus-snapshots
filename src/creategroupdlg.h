/*
 * creategroupdlg.h
 * Copyright (C) 2014  Ivan Romanov <drizt@land.ru>
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
 * along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#ifndef CREATEGROUPDLG_H
#define CREATEGROUPDLG_H

#include <QDialog>

class PsiContact;

class CreateGroupDlg : public QDialog
{
	Q_OBJECT

public:
	CreateGroupDlg(PsiContact *contact, QWidget *parent = 0);
	~CreateGroupDlg();

public slots:
	void accept();
	void checkGroupName();
	void addGroup();

private:
	class Private;
	Private *d;
};

#endif // CREATEGROUPDLG_H
