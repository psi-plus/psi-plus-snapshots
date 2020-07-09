/* stringprep.h --- Header file for stringprep functions.
   Copyright (C) 2002-2016 Simon Josefsson

   This file is part of GNU Libidn.

   GNU Libidn is free software: you can redistribute it and/or
   modify it under the terms of either:

     * the GNU Lesser General Public License as published by the Free
       Software Foundation; either version 3 of the License, or (at
       your option) any later version.

   or

     * the GNU General Public License as published by the Free
       Software Foundation; either version 2 of the License, or (at
       your option) any later version.

   or both in parallel, as here.

   GNU Libidn is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received copies of the GNU General Public License and
   the GNU Lesser General Public License along with this program.  If
   not, see <http://www.gnu.org/licenses/>. */

#ifndef STRINGPREP_H
#define STRINGPREP_H

#include <QtCore/QtGlobal>

#if defined(QSTRINGPREP_BUILDING)
#define IDNAPI Q_DECL_EXPORT
#else
#define IDNAPI Q_DECL_IMPORT
#endif

#include <stddef.h> /* size_t */
#include <stdint.h> /* uint32_t */

class QString;

#define STRINGPREP_VERSION "1.35"

/* Error codes. */
typedef enum {
    STRINGPREP_OK = 0,
    /* Stringprep errors. */
    STRINGPREP_CONTAINS_UNASSIGNED      = 1,
    STRINGPREP_CONTAINS_PROHIBITED      = 2,
    STRINGPREP_BIDI_BOTH_L_AND_RAL      = 3,
    STRINGPREP_BIDI_LEADTRAIL_NOT_RAL   = 4,
    STRINGPREP_BIDI_CONTAINS_PROHIBITED = 5,
    /* Error in calling application. */
    STRINGPREP_TOO_SMALL_BUFFER = 100,
    STRINGPREP_PROFILE_ERROR    = 101,
    STRINGPREP_FLAG_ERROR       = 102,
    STRINGPREP_UNKNOWN_PROFILE  = 103,
    STRINGPREP_ICONV_ERROR      = 104,
    /* Internal errors. */
    STRINGPREP_NFKC_FAILED  = 200,
    STRINGPREP_MALLOC_ERROR = 201
} Stringprep_rc;

enum Stringprep_profile_flags { STRINGPREP_NO_NFKC = 1, STRINGPREP_NO_BIDI = 2, STRINGPREP_NO_UNASSIGNED = 4 };

/* Steps in a stringprep profile. */
typedef enum {
    STRINGPREP_NFKC                = 1,
    STRINGPREP_BIDI                = 2,
    STRINGPREP_MAP_TABLE           = 3,
    STRINGPREP_UNASSIGNED_TABLE    = 4,
    STRINGPREP_PROHIBIT_TABLE      = 5,
    STRINGPREP_BIDI_PROHIBIT_TABLE = 6,
    STRINGPREP_BIDI_RAL_TABLE      = 7,
    STRINGPREP_BIDI_L_TABLE        = 8
} Stringprep_profile_steps;

#define STRINGPREP_MAX_MAP_CHARS 4

struct Stringprep_table_element {
    uint32_t start;
    uint32_t end                           = 0;  /* 0 if only one character */
    uint32_t map[STRINGPREP_MAX_MAP_CHARS] = {}; /* NULL if end is not 0 */
};
typedef struct Stringprep_table_element Stringprep_table_element;

struct Stringprep_table {
    Stringprep_table(int operation, long flags = 0, const Stringprep_table_element *table = nullptr,
                     size_t table_size = 0) :
        operation(Stringprep_profile_steps(operation)),
        flags(Stringprep_profile_flags(flags)), table(table), table_size(table_size)
    {
    }

    Stringprep_profile_steps        operation  = Stringprep_profile_steps(0);
    Stringprep_profile_flags        flags      = Stringprep_profile_flags(0);
    const Stringprep_table_element *table      = nullptr;
    size_t                          table_size = 0;
};
typedef struct Stringprep_table Stringprep_profile;

struct Stringprep_profiles {
    const char *              name;
    const Stringprep_profile *tables;
};
typedef struct Stringprep_profiles Stringprep_profiles;

extern IDNAPI const Stringprep_profiles stringprep_profiles[];

/* Profiles */
extern IDNAPI const Stringprep_table_element stringprep_rfc3454_A_1[];
extern IDNAPI const Stringprep_table_element stringprep_rfc3454_B_1[];
extern IDNAPI const Stringprep_table_element stringprep_rfc3454_B_2[];
extern IDNAPI const Stringprep_table_element stringprep_rfc3454_B_3[];
extern IDNAPI const Stringprep_table_element stringprep_rfc3454_C_1_1[];
extern IDNAPI const Stringprep_table_element stringprep_rfc3454_C_1_2[];
extern IDNAPI const Stringprep_table_element stringprep_rfc3454_C_2_1[];
extern IDNAPI const Stringprep_table_element stringprep_rfc3454_C_2_2[];
extern IDNAPI const Stringprep_table_element stringprep_rfc3454_C_3[];
extern IDNAPI const Stringprep_table_element stringprep_rfc3454_C_4[];
extern IDNAPI const Stringprep_table_element stringprep_rfc3454_C_5[];
extern IDNAPI const Stringprep_table_element stringprep_rfc3454_C_6[];
extern IDNAPI const Stringprep_table_element stringprep_rfc3454_C_7[];
extern IDNAPI const Stringprep_table_element stringprep_rfc3454_C_8[];
extern IDNAPI const Stringprep_table_element stringprep_rfc3454_C_9[];
extern IDNAPI const Stringprep_table_element stringprep_rfc3454_D_1[];
extern IDNAPI const Stringprep_table_element stringprep_rfc3454_D_2[];

/* Nameprep */

extern IDNAPI const Stringprep_profile stringprep_nameprep[];

#define stringprep_nameprep(in, maxlen) stringprep(in, maxlen, 0, stringprep_nameprep)

#define stringprep_nameprep_no_unassigned(in, maxlen)                                                                  \
    stringprep(in, maxlen, STRINGPREP_NO_UNASSIGNED, stringprep_nameprep)

/* SASL */

extern IDNAPI const Stringprep_profile       stringprep_saslprep[];
extern IDNAPI const Stringprep_table_element stringprep_saslprep_space_map[];
extern IDNAPI const Stringprep_profile       stringprep_plain[];
extern IDNAPI const Stringprep_profile       stringprep_trace[];

#define stringprep_plain(in, maxlen) stringprep(in, maxlen, 0, stringprep_plain)

/* Kerberos */

extern IDNAPI const Stringprep_profile stringprep_kerberos5[];

#define stringprep_kerberos5(in, maxlen) stringprep(in, maxlen, 0, stringprep_kerberos5)

/* XMPP */

extern IDNAPI const Stringprep_profile       stringprep_xmpp_nodeprep[];
extern IDNAPI const Stringprep_profile       stringprep_xmpp_resourceprep[];
extern IDNAPI const Stringprep_table_element stringprep_xmpp_nodeprep_prohibit[];

#define stringprep_xmpp_nodeprep(in, maxlen) stringprep(in, maxlen, 0, stringprep_xmpp_nodeprep)
#define stringprep_xmpp_resourceprep(in, maxlen) stringprep(in, maxlen, 0, stringprep_xmpp_resourceprep)

/* iSCSI */

extern IDNAPI const Stringprep_profile       stringprep_iscsi[];
extern IDNAPI const Stringprep_table_element stringprep_iscsi_prohibit[];

#define stringprep_iscsi(in, maxlen) stringprep(in, maxlen, 0, stringprep_iscsi)

/* API */

extern IDNAPI int stringprep_4i(QString &input, Stringprep_profile_flags flags, const Stringprep_profile *profile);
extern IDNAPI int stringprep(QString &input, Stringprep_profile_flags flags, const Stringprep_profile *profile);

extern IDNAPI int stringprep_profile(const char *in, char **out, const char *profile, Stringprep_profile_flags flags);

#endif /* STRINGPREP_H */
