/*
   Copyright (C) 2010 Red Hat, Inc.
   Copyright © 2006-2010 Collabora Ltd. <http://www.collabora.co.uk/>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#include "config.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib-object.h>
#include "spice-version.h"
#include "spice-util-priv.h"
#include "spice-util.h"

/**
 * SECTION:spice-util
 * @short_description: version and debugging functions
 * @title: Utilities
 * @section_id:
 * @stability: Stable
 * @include: spice-client.h
 *
 * Various functions for debugging and informational purposes.
 */

/**
 * spice_util_set_debug:
 * @enabled: %TRUE or %FALSE
 *
 * Enable or disable Spice-GTK debugging messages.
 * Kept for ABI compatibility.
 *
 * Deprecated: 0.43: Use spice_glib_set_debug() instead
 **/
void spice_util_set_debug(gboolean enabled)
{
    spice_glib_set_debug(enabled);
}

gboolean spice_util_get_debug(void)
{
    return spice_glib_get_debug();
}

/**
 * spice_util_get_version_string:
 *
 * Gets the version string
 *
 * Returns: Spice-GTK version as a const string.
 *
 * Deprecated: 0.43: Use spice_glib_get_version_string() instead
 **/
const gchar *spice_util_get_version_string(void)
{
    return spice_glib_get_version_string();
}

G_GNUC_INTERNAL
gboolean spice_strv_contains(const GStrv strv, const gchar *str)
{
    int i;

    if (strv == NULL)
        return FALSE;

    for (i = 0; strv[i] != NULL; i++)
        if (g_str_equal(strv[i], str))
            return TRUE;

    return FALSE;
}

/**
 * spice_uuid_to_string:
 * @uuid: (array fixed-size=16): UUID byte array
 *
 * Creates a string representation of @uuid, of the form
 * "06e023d5-86d8-420e-8103-383e4566087a"
 *
 * Returns: A string that should be freed with g_free().
 * Since: 0.22
 *
 * Deprecated: 0.43: Use spice_glib_uuid_to_string() instead
 **/
gchar* spice_uuid_to_string(const guint8 uuid[16])
{
    return spice_glib_uuid_to_string(uuid);
}

/**
  * spice_g_signal_connect_object: (skip)
  * @instance: the instance to connect to.
  * @detailed_signal: a string of the form "signal-name::detail".
  * @c_handler: the #GCallback to connect.
  * @gobject: the object to pass as data to @c_handler.
  * @connect_flags: a combination of #GConnectFlags.
  *
  * Similar to g_signal_connect_object() but will delete connection
  * when any of the objects is destroyed.
  *
  * Returns: the handler id.
  *
  * Deprecated: 0.43: Use spice_glib_signal_connect_object() instead
  */
gulong spice_g_signal_connect_object (gpointer instance,
                                      const gchar *detailed_signal,
                                      GCallback c_handler,
                                      gpointer gobject,
                                      GConnectFlags connect_flags)
{
    return spice_glib_signal_connect_object(instance, detailed_signal, c_handler,
                                            gobject, connect_flags);
}

G_GNUC_INTERNAL
const gchar* spice_yes_no(gboolean value)
{
    return value ? "yes" : "no";
}

G_GNUC_INTERNAL
guint16 spice_make_scancode(guint scancode, gboolean release)
{
    SPICE_DEBUG("%s: %s scancode %u",
                __FUNCTION__, release ? "release" : "", scancode);

    scancode &= 0x37f;
    if (release)
        scancode |= 0x80;
    if (scancode < 0x100)
        return scancode;
    return GUINT16_SWAP_LE_BE(0xe000 | (scancode - 0x100));
}

typedef enum {
    NEWLINE_TYPE_LF,
    NEWLINE_TYPE_CR_LF
} NewlineType;

static gssize get_line(const gchar *str, gsize len,
                       NewlineType type, gsize *nl_len)
{
    const gchar *p, *endl;
    gsize nl = 0;

    endl = (type == NEWLINE_TYPE_CR_LF) ? "\r\n" : "\n";
    p = g_strstr_len(str, len, endl);
    if (p) {
        len = p - str;
        nl = strlen(endl);
    }

    *nl_len = nl;
    return len;
}


static gchar* spice_convert_newlines(const gchar *str, gssize len,
                                     NewlineType from,
                                     NewlineType to)
{
    gssize length;
    gsize nl;
    GString *output;
    gint i;

    g_return_val_if_fail(str != NULL, NULL);
    g_return_val_if_fail(len >= -1, NULL);
    /* only 2 supported combinations */
    g_return_val_if_fail((from == NEWLINE_TYPE_LF &&
                          to == NEWLINE_TYPE_CR_LF) ||
                         (from == NEWLINE_TYPE_CR_LF &&
                          to == NEWLINE_TYPE_LF), NULL);

    if (len == -1)
        len = strlen(str);
    /* sometime we get \0 terminated strings, skip that, or it fails
       to utf8 validate line with \0 end */
    else if (len > 0 && str[len-1] == 0)
        len -= 1;

    /* allocate worst case, if it's small enough, we don't care much,
     * if it's big, malloc will put us in mmap'd region, and we can
     * over allocate.
     */
    output = g_string_sized_new(len * 2 + 1);

    for (i = 0; i < len; i += length + nl) {
        length = get_line(str + i, len - i, from, &nl);
        if (length < 0)
            break;

        g_string_append_len(output, str + i, length);

        if (nl) {
            /* let's not double \r if it's already in the line */
            if (to == NEWLINE_TYPE_CR_LF &&
                (output->len == 0 || output->str[output->len - 1] != '\r'))
                g_string_append_c(output, '\r');

            g_string_append_c(output, '\n');
        }
    }

    return g_string_free_and_steal(output);
}

G_GNUC_INTERNAL
gchar* spice_dos2unix(const gchar *str, gssize len)
{
    return spice_convert_newlines(str, len,
                                  NEWLINE_TYPE_CR_LF,
                                  NEWLINE_TYPE_LF);
}

G_GNUC_INTERNAL
gchar* spice_unix2dos(const gchar *str, gssize len)
{
    return spice_convert_newlines(str, len,
                                  NEWLINE_TYPE_LF,
                                  NEWLINE_TYPE_CR_LF);
}

static bool buf_is_ones(unsigned size, const guint8 *data)
{
    int i;

    for (i = 0 ; i < size; ++i) {
        if (data[i] != 0xff) {
            return false;
        }
    }
    return true;
}

static bool is_edge_helper(const guint8 *xor, int bpl, int x, int y)
{
    return (xor[bpl * y + (x / 8)] & (0x80 >> (x % 8))) > 0;
}

static bool is_edge(unsigned width, unsigned height, const guint8 *xor, int bpl, int x, int y)
{
    if (x == 0 || x == width -1 || y == 0 || y == height - 1) {
        return 0;
    }
#define P(x, y) is_edge_helper(xor, bpl, x, y)
    return !P(x, y) && (P(x - 1, y + 1) || P(x, y + 1) || P(x + 1, y + 1) ||
                        P(x - 1, y)     ||                P(x + 1, y)     ||
                        P(x - 1, y - 1) || P(x, y - 1) || P(x + 1, y - 1));
#undef P
}

/* Mono cursors have two places, "and" and "xor". If a bit is 1 in both, it
 * means invertion of the corresponding pixel in the display. Since X11 (and
 * gdk) doesn't do invertion, instead we do edge detection and turn the
 * sorrounding edge pixels black, and the invert-me pixels white. To
 * illustrate:
 *
 *  and   xor      dest RGB (1=0xffffff, 0=0x000000)
 *
 *                        dest alpha (1=0xff, 0=0x00)
 *
 * 11111 00000     00000  00000
 * 11111 00000     00000  01110
 * 11111 00100 =>  00100  01110
 * 11111 00100     00100  01110
 * 11111 00000     00000  01110
 * 11111 00000     00000  00000
 *
 * See tests/util.c for more tests
 *
 * Notes:
 *  Assumes width >= 8 (i.e. bytes per line is at least 1)
 *  Assumes edges are not on the boundary (first/last line/column) for simplicity
 *
 */
G_GNUC_INTERNAL
void spice_mono_edge_highlight(unsigned width, unsigned height,
                               const guint8 *and, const guint8 *xor, guint8 *dest)
{
    int bpl = (width + 7) / 8;
    bool and_ones = buf_is_ones(height * bpl, and);
    int x, y, bit;
    const guint8 *xor_base = xor;

    for (y = 0; y < height; y++) {
        bit = 0x80;
        for (x = 0; x < width; x++, dest += 4) {
            if (is_edge(width, height, xor_base, bpl, x, y) && and_ones) {
                dest[0] = 0x00;
                dest[1] = 0x00;
                dest[2] = 0x00;
                dest[3] = 0xff;
                goto next_bit;
            }
            if (and[x/8] & bit) {
                if (xor[x/8] & bit) {
                    dest[0] = 0xff;
                    dest[1] = 0xff;
                    dest[2] = 0xff;
                    dest[3] = 0xff;
                } else {
                    /* unchanged -> transparent */
                    dest[0] = 0x00;
                    dest[1] = 0x00;
                    dest[2] = 0x00;
                    dest[3] = 0x00;
                }
            } else {
                if (xor[x/8] & bit) {
                    /* set -> white */
                    dest[0] = 0xff;
                    dest[1] = 0xff;
                    dest[2] = 0xff;
                    dest[3] = 0xff;
                } else {
                    /* clear -> black */
                    dest[0] = 0x00;
                    dest[1] = 0x00;
                    dest[2] = 0x00;
                    dest[3] = 0xff;
                }
            }
        next_bit:
            bit >>= 1;
            if (bit == 0) {
                bit = 0x80;
            }
        }
        and += bpl;
        xor += bpl;
    }
}
