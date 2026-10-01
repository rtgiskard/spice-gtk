/*
   Copyright (C) 2010 Red Hat, Inc.

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

/* system */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>

#include "common/mem.h"
#include "common/messages.h"
#include "common/marshaller.h"

#include "spice-version.h"
#include "spice-util.h"

/* Display-channel object data shared by the core and widget libraries.
 * Borrowed from the decoder; cleared before the pipeline is released. */
#define SPICE_DISPLAY_NATIVE_PIPELINE "spice-display-native-pipeline"
