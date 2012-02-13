/*
 * xmpp_receipts.h - XEP-0184 support helper file
 * Copyright (C) 2007  Michail Pishchagin
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

#ifndef XMPP_RECEIPTS_H
#define XMPP_RECEIPTS_H

namespace XMPP {
	typedef enum {
		ReceiptNone,
		ReceiptRequest,
		ReceiptReceived
	} MessageReceipt;
}

#endif
