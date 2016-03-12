/*
 * xmpp_status.h
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

#ifndef XMPP_STATUS_H
#define XMPP_STATUS_H

#include <QList>
#include <QString>
#include <QDateTime>
#include <QCryptographicHash>

#include "xmpp_muc.h"
#include "xmpp_bitsofbinary.h"

namespace XMPP
{
	class DiscoItem;
	class CapsSpec
	{
		public:
			typedef QMap<QString,QCryptographicHash::Algorithm> CryptoMap;
			static const QCryptographicHash::Algorithm invalidAlgo = (QCryptographicHash::Algorithm)255;

			CapsSpec();
			CapsSpec(const QString& node, QCryptographicHash::Algorithm hashAlgo, const QString& ver = QString::null);
			CapsSpec(const DiscoItem &disco, QCryptographicHash::Algorithm hashAlgo = QCryptographicHash::Sha1);

			bool isValid() const;
			const QString& node() const;
			const QString& version() const;
			QCryptographicHash::Algorithm hashAlgorithm() const;
			QString flatten() const;

			void resetVersion();

			bool operator==(const CapsSpec&) const;
			bool operator!=(const CapsSpec&) const;
			bool operator<(const CapsSpec&) const;

			QDomElement toXml(QDomDocument *doc) const;
			static CapsSpec fromXml(const QDomElement &e);

			static CryptoMap &cryptoMap();

		private:
			QString node_, ver_;
			QCryptographicHash::Algorithm hashAlgo_;
	};

	class StatusPrivate;

	class Status
	{
	public:
		enum Type { Offline, Online, Away, XA, DND, Invisible, FFC };

		Status(const QString &show=QString(), const QString &status=QString(), int priority=0, bool available=true);
		Status(Type type, const QString& status=QString(), int priority=0);
		Status(const Status &);
		Status &operator=(const Status &);
		~Status();

		int priority() const;
		Type type() const;
		QString typeString() const;
		const QString & show() const;
		const QString & status() const;
		QDateTime timeStamp() const;
		const QString & keyID() const;
		bool isAvailable() const;
		bool isAway() const;
		bool isInvisible() const;
		bool hasError() const;
		int errorCode() const;
		const QString & errorString() const;

		const QString & xsigned() const;
		const QString & songTitle() const;
		const CapsSpec & caps() const;

		bool isMUC() const;
		bool hasMUCItem() const;
		const MUCItem & mucItem() const;
		bool hasMUCDestroy() const;
		const MUCDestroy & mucDestroy() const;
		const QList<int>& getMUCStatuses() const;
		const QString& mucPassword() const;
		bool hasMUCHistory() const;
		int mucHistoryMaxChars() const;
		int mucHistoryMaxStanzas() const;
		int mucHistorySeconds() const;
		const QDateTime & mucHistorySince() const;

		static Type txt2type(const QString& stat);

		void setPriority(int);
		void setType(Type);
		void setType(QString);
		void setShow(const QString &);
		void setStatus(const QString &);
		void setTimeStamp(const QDateTime &);
		void setKeyID(const QString &);
		void setIsAvailable(bool);
		void setIsInvisible(bool);
		void setError(int, const QString &);
		void setCaps(const CapsSpec&);

		void setMUC();
		void setMUCItem(const MUCItem&);
		void setMUCDestroy(const MUCDestroy&);
		void addMUCStatus(int);
		void setMUCPassword(const QString&);
		void setMUCHistory(int maxchars, int maxstanzas, int seconds, const QDateTime &since);

		void setXSigned(const QString &);
		void setSongTitle(const QString &);

		// JEP-153: VCard-based Avatars
		const QString& photoHash() const;
		void setPhotoHash(const QString&);
		bool hasPhotoHash() const;

		// XEP-0231 bits of binary
		void addBoBData(const BoBData &);
		QList<BoBData> bobDataList() const;

	private:
		QSharedDataPointer<StatusPrivate> d;
	};

}

#endif
