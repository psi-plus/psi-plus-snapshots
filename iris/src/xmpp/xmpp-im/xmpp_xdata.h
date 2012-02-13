/*
 * xmpp_xdata.h - a class for jabber:x:data forms
 * Copyright (C) 2003-2004  Michail Pishchagin
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#ifndef XMPPXDATA_H
#define XMPPXDATA_H

#include <QString>
#include <QMap>
#include <QList>
#include <QSharedDataPointer>
#include <QStringList>
#include <QSize>
#include <QHash>

class QDomElement;
class QDomDocument;

namespace XMPP {

	class XData
	{
	public:
		XData();

		QString title() const;
		void setTitle(const QString &);

		QString instructions() const;
		void setInstructions(const QString &);

		enum Type {
			Data_Form,
			Data_Result,
			Data_Submit,
			Data_Cancel
		};

		Type type() const;
		void setType(Type);
		QString registrarType() const;

		struct ReportField {
			ReportField() { }
			ReportField( QString _label, QString _name ) { label = _label; name = _name; }
			QString label;
			QString name;
		};
		const QList<ReportField> &report() const;

		typedef QMap<QString, QString> ReportItem;
		const QList<ReportItem> &reportItems() const;

		void fromXml(const QDomElement &);
		QDomElement toXml(QDomDocument *, bool submitForm = true) const;
		bool isValid() const;

	public:
		class Field {
		public:
			Field();
			~Field();

			QString desc() const;
			void setDesc(const QString &);

			struct Option {
				QString label;
				QString value;
			};

			struct MediaUri {
				QString type;
				QString uri;
				QHash<QString,QString> params;
			};

			class MediaElement : public QList<MediaUri>
			{
			public:
				void append(const QString &type, const QString &uri, QHash<QString,QString> params);
				void setMediaSize(const QSize &size);
				QSize mediaSize() const;
				bool checkSupport(const QStringList &wildcards);

			private:
				QSize _size;
			};

			typedef QList<Option> OptionList;
			OptionList options() const;
			void setOptions(OptionList);

			MediaElement mediaElement() const;
			void setMediaElement(const MediaElement &);

			bool required() const;
			void setRequired(bool);

			QString label() const;
			void setLabel(const QString &);

			QString var() const;
			void setVar(const QString &);

			// generic value variable, because every possible Type
			// can be converted to QStringList. Field_Single will
			// use just one string in QStringList. Field_Boolean will
			// use just one string, and that string will equal 0 or 1.
			// and so on...
			QStringList value() const;
			void setValue(const QStringList &);

			enum Type {
				Field_Boolean,
				Field_Fixed,
				Field_Hidden,
				Field_JidMulti,
				Field_JidSingle,
				Field_ListMulti,
				Field_ListSingle,
				Field_TextMulti,
				Field_TextPrivate,
				Field_TextSingle
			};

			Type type() const;
			void setType(Type);

			bool isValid() const;

			void fromXml(const QDomElement &);
			QDomElement toXml(QDomDocument *, bool submitForm = true) const;

		private:
			QString _desc, _label, _var;
			QList<Option> _options;
			MediaElement _mediaElement;
			bool _required;
			Type _type;
			QStringList _value;
		};

		typedef QList<Field> FieldList;

		FieldList fields() const;
		void setFields(const FieldList &);

	private:
		class Private : public QSharedData {
		public:
			QString title, instructions;
			XData::Type type;
			QString registrarType;
			FieldList fields;
			QList<ReportField> report;
			QList<ReportItem>  reportItems;
		};
		QSharedDataPointer<Private> d;
	};

};

#endif
