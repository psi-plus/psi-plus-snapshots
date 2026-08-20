/*
 * im.h - XMPP "IM" library API
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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef XMPP_IM_H
#define XMPP_IM_H

#include <iris/jid/jid.h>
#include <iris/xmpp-core/xmpp.h>
#include <iris/xmpp-im/xmpp_address.h>
#include <iris/xmpp-im/xmpp_agentitem.h>
#include <iris/xmpp-im/xmpp_chatstate.h>
#include <iris/xmpp-im/xmpp_client.h>
#include <iris/xmpp-im/xmpp_discoitem.h>
#include <iris/xmpp-im/xmpp_encryption.h>
#include <iris/xmpp-im/xmpp_features.h>
#include <iris/xmpp-im/xmpp_form.h>
#include <iris/xmpp-im/xmpp_hash.h>
#include <iris/xmpp-im/xmpp_htmlelement.h>
#include <iris/xmpp-im/xmpp_httpauthrequest.h>
#include <iris/xmpp-im/xmpp_liveroster.h>
#include <iris/xmpp-im/xmpp_liverosteritem.h>
#include <iris/xmpp-im/xmpp_message.h>
#include <iris/xmpp-im/xmpp_muc.h>
#include <iris/xmpp-im/xmpp_omemostorage.h>
#include <iris/xmpp-im/xmpp_pubsub.h>
#include <iris/xmpp-im/xmpp_pubsubevent.h>
#include <iris/xmpp-im/xmpp_pubsubitem.h>
#include <iris/xmpp-im/xmpp_pubsubretraction.h>
#include <iris/xmpp-im/xmpp_sce.h>
#ifdef IRIS_ENABLE_OMEMO
#include <iris/xmpp-im/xmpp_omemo.h>
#endif
#include <iris/xmpp-im/xmpp_resource.h>
#include <iris/xmpp-im/xmpp_resourcelist.h>
#include <iris/xmpp-im/xmpp_roster.h>
#include <iris/xmpp-im/xmpp_rosteritem.h>
#include <iris/xmpp-im/xmpp_rosterx.h>
#include <iris/xmpp-im/xmpp_status.h>
#include <iris/xmpp-im/xmpp_task.h>
#include <iris/xmpp-im/xmpp_thumbs.h>
#include <iris/xmpp-im/xmpp_url.h>
#include <iris/xmpp-im/xmpp_xdata.h>

#endif // XMPP_IM_H
