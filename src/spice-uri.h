/*
   Copyright (C) 2012 Red Hat, Inc.

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
#pragma once

#if !defined(__SPICE_CLIENT_H_INSIDE__) && !defined(SPICE_COMPILATION)
#warning "Only <spice-client.h> can be included directly"
#endif

#include <glib-object.h>

G_BEGIN_DECLS

#define SPICE_TYPE_URI (spice_uri_get_type ())
#define SPICE_URI(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), SPICE_TYPE_URI, SpiceURI))
#define SPICE_URI_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), SPICE_TYPE_URI, SpiceURIClass))
#define SPICE_IS_URI(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), SPICE_TYPE_URI))
#define SPICE_IS_URI_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), SPICE_TYPE_URI))
#define SPICE_URI_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), SPICE_TYPE_URI, SpiceURIClass))

/**
 * SpiceURI:
 *
 * The #SpiceURI struct is opaque and cannot be accessed directly.
 */
typedef struct _SpiceURI SpiceURI;

/**
 * SpiceURIClass:
 *
 * The #SpiceURIClass struct is opaque and cannot be accessed directly.
 * It is class structure for #SpiceURI.
 */
typedef struct _SpiceURIClass SpiceURIClass;
typedef struct _SpiceURIPrivate SpiceURIPrivate;

SPICE_GTK_AVAILABLE_IN_0_24
GType spice_uri_get_type(void);

SPICE_GTK_AVAILABLE_IN_0_24
const gchar* spice_uri_get_scheme(SpiceURI* uri);
SPICE_GTK_AVAILABLE_IN_0_24
void spice_uri_set_scheme(SpiceURI* uri, const gchar* scheme);
SPICE_GTK_AVAILABLE_IN_0_24
const gchar* spice_uri_get_hostname(SpiceURI* uri);
SPICE_GTK_AVAILABLE_IN_0_24
void spice_uri_set_hostname(SpiceURI* uri, const gchar* hostname);
SPICE_GTK_AVAILABLE_IN_0_24
guint spice_uri_get_port(SpiceURI* uri);
SPICE_GTK_AVAILABLE_IN_0_24
void spice_uri_set_port(SpiceURI* uri, guint port);
SPICE_GTK_AVAILABLE_IN_0_24
gchar *spice_uri_to_string(SpiceURI* uri);
SPICE_GTK_AVAILABLE_IN_0_24
const gchar* spice_uri_get_user(SpiceURI* uri);
SPICE_GTK_AVAILABLE_IN_0_24
void spice_uri_set_user(SpiceURI* uri, const gchar* user);
SPICE_GTK_AVAILABLE_IN_0_24
const gchar* spice_uri_get_password(SpiceURI* uri);
SPICE_GTK_AVAILABLE_IN_0_24
void spice_uri_set_password(SpiceURI* uri, const gchar* password);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SpiceURI, g_object_unref)
G_END_DECLS
