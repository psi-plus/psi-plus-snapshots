/*
 * contactlistitem.cpp - base class for contact list items
 * Copyright (C) 2008-2010  Yandex LLC (Michail Pishchagin)
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

#include "contactlistitem.h"

#include "psicontact.h"
#include "psiaccount.h"

class ContactListItem::Private
{
public:
	Private();
	bool editing;
	PsiAccount *account;
};

ContactListItem::Private::Private()
	: editing(false)
	, account(0)
{
}

ContactListItem::ContactListItem(PsiAccount *account, QObject* parent)
	: QObject(parent)
	, d(new ContactListItem::Private)
{
	d->account = account;
}

ContactListItem::~ContactListItem()
{
	delete d;
}

bool ContactListItem::isEditable() const
{
	return false;
}

bool ContactListItem::isDragEnabled() const
{
	return isEditable();
}

bool ContactListItem::isRemovable() const
{
	return false;
}

bool ContactListItem::isExpandable() const
{
	return false;
}

bool ContactListItem::expanded() const
{
	return false;
}

void ContactListItem::setExpanded(bool expanded)
{
	Q_UNUSED(expanded);
}

ContactListItemMenu* ContactListItem::contextMenu(ContactListModel* model)
{
	Q_UNUSED(model);
	return 0;
}

bool ContactListItem::isFixedSize() const
{
	return true;
}

bool ContactListItem::compare(const ContactListItem* other) const
{
	const PsiContact* left  = dynamic_cast<const PsiContact*>(this);
	const PsiContact* right = dynamic_cast<const PsiContact*>(other);
	if (!left ^ !right) {
		return !right;
	}
	return comparisonName() < other->comparisonName();
}

QString ContactListItem::comparisonName() const
{
	return name();
}

bool ContactListItem::editing() const
{
	return d->editing;
}

void ContactListItem::setEditing(bool editing)
{
	d->editing = editing;
}

const QString& ContactListItem::displayName() const
{
	return name();
}

PsiAccount *ContactListItem::account() const
{
	return d->account;
}
