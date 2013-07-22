/*
 * jid.cpp - class for verifying and manipulating XMPP addresses
 * Copyright (C) 2003  Justin Karneges
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

//#undef IRIS_XMPP_JID_DEPRECATED
#include "xmpp/jid/jid.h"

#include <QCoreApplication>
#include <stringprep.h>

#ifndef NO_IRISNET
#include "irisnetglobal_p.h"
#endif

using namespace XMPP;


//----------------------------------------------------------------------------
// StringPrepCache
//----------------------------------------------------------------------------
	QScopedPointer<StringPrepCache> StringPrepCache::_instance;

	bool StringPrepCache::nameprep(const QString &in, int maxbytes, QString& out)
	{
		if (in.trimmed().isEmpty()) {
			out = QString();
			return false; // empty names or just spaces are disallowed (rfc5892+rfc6122)
		}

		StringPrepCache *that = instance();

		Result *r = that->nameprep_table[in];
		if (r) {
			if(!r->norm) {
				return false;
			}
			out = *(r->norm);
			return true;
		}

		QByteArray cs = in.toUtf8();
		cs.resize(maxbytes);
		if(stringprep(cs.data(), maxbytes, (Stringprep_profile_flags)0, stringprep_nameprep) != 0)
		{
			that->nameprep_table.insert(in, new Result);
			return false;
		}

		QString norm = QString::fromUtf8(cs);
		that->nameprep_table.insert(in, new Result(norm));
		out = norm;
		return true;
	}

	bool StringPrepCache::nodeprep(const QString &in, int maxbytes, QString& out)
	{
		if(in.isEmpty()) {
      out = QString();
			return true;
		}

		StringPrepCache *that = instance();

		Result *r = that->nodeprep_table[in];
		if(r) {
			if(!r->norm) {
				return false;
			}
			out = *(r->norm);
			return true;
		}

		QByteArray cs = in.toUtf8();
		cs.resize(maxbytes);
		if(stringprep(cs.data(), maxbytes, (Stringprep_profile_flags)0, stringprep_xmpp_nodeprep) != 0) {
			that->nodeprep_table.insert(in, new Result);
			return false;
		}

		QString norm = QString::fromUtf8(cs);
		that->nodeprep_table.insert(in, new Result(norm));
		out = norm;
		return true;
	}

	bool StringPrepCache::resourceprep(const QString &in, int maxbytes, QString& out)
	{
		if(in.isEmpty()) {
      out = QString();
			return true;
		}

		StringPrepCache *that = instance();

		Result *r = that->resourceprep_table[in];
		if(r) {
			if(!r->norm) {
				return false;
			}
			out = *(r->norm);
			return true;
		}

		QByteArray cs = in.toUtf8();
		cs.resize(maxbytes);
		if(stringprep(cs.data(), maxbytes, (Stringprep_profile_flags)0, stringprep_xmpp_resourceprep) != 0) {
			that->resourceprep_table.insert(in, new Result);
			return false;
		}

		QString norm = QString::fromUtf8(cs);
		that->resourceprep_table.insert(in, new Result(norm));
		out = norm;
		return true;
	}

	bool StringPrepCache::saslprep(const QString &in, int maxbytes, QString& out)
	{
		if(in.isEmpty()) {
	  out = QString();
			return true;
		}

		StringPrepCache *that = instance();

		Result *r = that->saslprep_table[in];
		if(r) {
			if(!r->norm) {
				return false;
			}
			out = *(r->norm);
			return true;
		}

		QByteArray cs = in.toUtf8();
		cs.resize(maxbytes);
		if(stringprep(cs.data(), maxbytes, (Stringprep_profile_flags)0, stringprep_saslprep) != 0) {
			that->saslprep_table.insert(in, new Result);
			return false;
		}

		QString norm = QString::fromUtf8(cs);
		that->saslprep_table.insert(in, new Result(norm));
		out = norm;
		return true;
	}

	void StringPrepCache::cleanup()
	{
		_instance.reset(0);
	}

	StringPrepCache *StringPrepCache::instance()
	{
		if(!_instance)
		{
			_instance.reset(new StringPrepCache);
#ifndef NO_IRISNET
			irisNetAddPostRoutine(cleanup);
#endif
		}
		return _instance.data();
	}

	StringPrepCache::StringPrepCache()
	{
	}

	StringPrepCache::~StringPrepCache()
	{
		foreach(Result* r, nameprep_table) {
			delete r;
		}
		nameprep_table.clear();
		foreach(Result* r, nodeprep_table) {
			delete r;
		}
		nodeprep_table.clear();
		foreach(Result* r, resourceprep_table) {
			delete r;
		}
		resourceprep_table.clear();
		foreach(Result* r, saslprep_table) {
			delete r;
		}
		saslprep_table.clear();
	}

//----------------------------------------------------------------------------
// Jid
//----------------------------------------------------------------------------
//
static inline bool validDomain(const QString &s, QString& norm)
{
	return StringPrepCache::nameprep(s, 1024, norm);
}

static inline bool validNode(const QString &s, QString& norm)
{
	return StringPrepCache::nodeprep(s, 1024, norm);
}

static inline bool validResource(const QString &s, QString& norm)
{
	return StringPrepCache::resourceprep(s, 1024, norm);
}

Jid::Jid()
{
	valid = false;
	null = true;
}

Jid::~Jid()
{
}

Jid::Jid(const QString &s)
{
	set(s);
}

Jid::Jid(const QString &node, const QString& domain, const QString& resource)
{
	set(domain, node, resource);
}

Jid::Jid(const char *s)
{
	set(QString(s));
}

Jid & Jid::operator=(const QString &s)
{
	set(s);
	return *this;
}

Jid & Jid::operator=(const char *s)
{
	set(QString(s));
	return *this;
}

void Jid::reset()
{
	f = QString();
	b = QString();
	d = QString();
	n = QString();
	r = QString();
	valid = false;
	null = true;
}

void Jid::update()
{
	// build 'bare' and 'full' jids
	if(n.isEmpty())
		b = d;
	else
		b = n + '@' + d;
	if(r.isEmpty())
		f = b;
	else
		f = b + '/' + r;
	if(f.isEmpty())
		valid = false;
	null = f.isEmpty() && r.isEmpty();
}

void Jid::set(const QString &s)
{
	QString rest, domain, node, resource;
	QString norm_domain, norm_node, norm_resource;
	int x = s.indexOf('/');
	if(x != -1) {
		rest = s.mid(0, x);
		resource = s.mid(x+1);
	}
	else {
		rest = s;
		resource = QString();
	}
	if(!validResource(resource, norm_resource)) {
		reset();
		return;
	}

	x = rest.indexOf('@');
	if(x != -1) {
		node = rest.mid(0, x);
		domain = rest.mid(x+1);
	}
	else {
		node = QString();
		domain = rest;
	}
	if(!validDomain(domain, norm_domain) || !validNode(node, norm_node)) {
		reset();
		return;
	}

	valid = true;
	null = false;
	d = norm_domain;
	n = norm_node;
	r = norm_resource;
	update();
}

void Jid::set(const QString &domain, const QString &node, const QString &resource)
{
	QString norm_domain, norm_node, norm_resource;
	if(!validDomain(domain, norm_domain) ||
			!validNode(node, norm_node) ||
			!validResource(resource, norm_resource))
	{
		reset();
		return;
	}
	valid = true;
	null = false;
	d = norm_domain;
	n = norm_node;
	r = norm_resource;
	update();
}

void Jid::setDomain(const QString &s)
{
	if(!valid)
		return;
	QString norm;
	if(!validDomain(s, norm)) {
		reset();
		return;
	}
	d = norm;
	update();
}

void Jid::setNode(const QString &s)
{
	if(!valid)
		return;
	QString norm;
	if(!validNode(s, norm)) {
		reset();
		return;
	}
	n = norm;
	update();
}

void Jid::setResource(const QString &s)
{
	if(!valid)
		return;
	QString norm;
	if(!validResource(s, norm)) {
		reset();
		return;
	}
	r = norm;
	update();
}

Jid Jid::withNode(const QString &s) const
{
	Jid j = *this;
	j.setNode(s);
	return j;
}

Jid Jid::withDomain(const QString &s) const
{
	Jid j = *this;
	j.setDomain(s);
	return j;
}

Jid Jid::withResource(const QString &s) const
{
	Jid j = *this;
	j.setResource(s);
	return j;
}

bool Jid::isValid() const
{
	return valid;
}

bool Jid::isEmpty() const
{
	return f.isEmpty();
}

bool Jid::compare(const Jid &a, bool compareRes) const
{
	if(null && a.null)
		return true;

	// only compare valid jids
	if(!valid || !a.valid)
		return false;

	if(compareRes ? (f != a.f) : (b != a.b))
		return false;

	return true;
}
