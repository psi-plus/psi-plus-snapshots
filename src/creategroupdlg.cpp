/*
 * creategroupdlg.cpp
 * Copyright (C) 20014  Ivan Romanov <drizt@land.ru>
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

#include "ui_creategroup.h"
#include "creategroupdlg.h"
#include "psicontact.h"
#include "userlist.h"
#include "psiaccount.h"

#include <QPushButton>
#include <QTreeWidgetItem>
#include <QList>
#include <QRegExp>

class CreateGroupDlg::Private
{
public:
	CreateGroupDlg *q;
	Ui::CreateGroupDlg *ui;
	PsiContact *contact;

	QString fullGroupName()
	{
		QStringList groupNameList;
		groupNameList << ui->lneGroupName->text();

		QTreeWidgetItem *item = ui->twGroups->currentItem();
		if (item != ui->twGroups->topLevelItem(0)) {
			while (item != 0) {
				groupNameList.prepend(item->text(0));
				item = item->parent();
			}
		}

		QString groupsDelimiter = contact->account()->userList()->groupsDelimiter();

		return groupNameList.join(groupsDelimiter);
	}
};

CreateGroupDlg::CreateGroupDlg(PsiContact *contact, QWidget *parent)
	: QDialog(parent)
	, d(new CreateGroupDlg::Private)
{
	d->q = this;
	d->ui = new Ui::CreateGroupDlg;
	d->ui->setupUi(this);
	d->ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
	d->ui->btnAddGroup->setEnabled(false);
	d->contact = contact;

	// Fill groups tree
	QString groupsDelimiter = d->contact->account()->userList()->groupsDelimiter();

	QList<QTreeWidgetItem *> items;
	items << new QTreeWidgetItem((QTreeWidget*)0, QStringList(tr("<None>")));

	// Show only <None> if no delimiter
	if (groupsDelimiter.indexOf(QRegExp("^[0-9A-z]?$")) == -1) {
		QStringList groupList = d->contact->account()->groupList();
		groupList.removeAll("");
		groupList.removeAll(PsiContact::hiddenGroupName());
		foreach (const QString &groupName, groupList) {
			QStringList subGroupList = groupName.split(groupsDelimiter);

			QTreeWidgetItem *parentItem = 0;
			foreach (const QString &subGroupName, subGroupList) {
				QList<QTreeWidgetItem*> children;
				if (parentItem) {
					for (int i = 0; i < parentItem->childCount(); i++)
						children << parentItem->child(i);
				}
				else {
					children = items;
				}

				bool needCreateItem = true;

				for (int i = 0; i < children.length(); i++) {
					if (children.at(i)->text(0) == subGroupName) {
						parentItem = children.at(i);
						needCreateItem = false;
						break;
					}
				}

				if (needCreateItem) {
					parentItem = new QTreeWidgetItem(parentItem, QStringList(subGroupName));
					items << parentItem;
				}
			}
		}
	}
	d->ui->twGroups->insertTopLevelItems(0, items);
	d->ui->twGroups->setCurrentItem(items.at(0));
}

CreateGroupDlg::~CreateGroupDlg()
{
	delete d->ui;
	delete d;
}

void CreateGroupDlg::accept()
{
	QDialog::accept();
	d->contact->setGroups(QStringList() << d->fullGroupName());
}

void CreateGroupDlg::checkGroupName()
{
	bool ok;
	QString groupName = d->fullGroupName();

	// Subgroup name can't be empty
	ok = !d->ui->lneGroupName->text().isEmpty();

	// Contact is not added to this group
	if (ok) {
		ok = !d->contact->userListItem().groups().contains(groupName);
	}

	d->ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(ok);

	// Can't create subgroup with delimiter
	QString subGroupName = d->ui->lneGroupName->text();
	if (ok) {
		QString groupsDelimiter = d->contact->account()->userList()->groupsDelimiter();
		if (groupsDelimiter.indexOf(QRegExp("^[0-9A-z]?$")) == -1) {
			if (subGroupName.contains(groupsDelimiter)) {
				ok = false;
			}
		}
	}

	// No such subgroup
	if (ok) {
		QTreeWidgetItem *item = d->ui->twGroups->currentItem();
		QList<QTreeWidgetItem*> children;

		if (item == d->ui->twGroups->topLevelItem(0)) {
			for (int i = 1; i < d->ui->twGroups->topLevelItemCount(); i++)
				children << d->ui->twGroups->topLevelItem(1);
		}
		else {
			for (int i = 0; i < item->childCount(); i++)
				children << item->child(i);
		}

		for (int i = 0; i < children.length(); i++) {
			if (children.at(i)->text(0) == subGroupName) {
				ok = false;
				break;
			}
		}
	}
	d->ui->btnAddGroup->setEnabled(ok);
}

void CreateGroupDlg::addGroup()
{
	QTreeWidgetItem *item = d->ui->twGroups->currentItem();
	QString subGroupName = d->ui->lneGroupName->text();

	if (item == d->ui->twGroups->topLevelItem(0)) {
		item = 0;
		item = new QTreeWidgetItem(item, QStringList(subGroupName));
		d->ui->twGroups->addTopLevelItem(item);
	}
	else {
		item = new QTreeWidgetItem(item, QStringList(subGroupName));
	}
	d->ui->twGroups->setCurrentItem(item);

	d->ui->lneGroupName->setText("");
	d->ui->lneGroupName->setFocus();
}
