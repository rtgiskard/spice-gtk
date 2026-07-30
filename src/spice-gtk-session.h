/*
   Copyright (C) 2010-2011 Red Hat, Inc.

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

#if !defined(__SPICE_CLIENT_GTK_H_INSIDE__) && !defined(SPICE_COMPILATION)
#warning "Only <spice-client-gtk.h> can be included directly"
#endif

#include "spice-client.h"

G_BEGIN_DECLS

#define SPICE_TYPE_GTK_SESSION            (spice_gtk_session_get_type ())
#define SPICE_GTK_SESSION(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), SPICE_TYPE_GTK_SESSION, SpiceGtkSession))
#define SPICE_GTK_SESSION_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), SPICE_TYPE_GTK_SESSION, SpiceGtkSessionClass))
#define SPICE_IS_GTK_SESSION(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), SPICE_TYPE_GTK_SESSION))
#define SPICE_IS_GTK_SESSION_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), SPICE_TYPE_GTK_SESSION))
#define SPICE_GTK_SESSION_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), SPICE_TYPE_GTK_SESSION, SpiceGtkSessionClass))

/**
 * SpiceGtkSession:
 *
 * The #SpiceGtkSession struct is opaque and should not be accessed directly.
 */
typedef struct _SpiceGtkSession SpiceGtkSession;

/**
 * SpiceGtkSessionClass:
 *
 * Class structure for #SpiceGtkSession. It is opaque and should not be accessed directly.
 */
typedef struct _SpiceGtkSessionClass SpiceGtkSessionClass;

SPICE_GTK_AVAILABLE_IN_0_8
GType spice_gtk_session_get_type(void);

SPICE_GTK_AVAILABLE_IN_0_8
SpiceGtkSession *spice_gtk_session_get(SpiceSession *session);
SPICE_GTK_AVAILABLE_IN_0_8
void spice_gtk_session_copy_to_guest(SpiceGtkSession *self);
SPICE_GTK_AVAILABLE_IN_0_8
void spice_gtk_session_paste_from_guest(SpiceGtkSession *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SpiceGtkSession, g_object_unref)
G_END_DECLS
