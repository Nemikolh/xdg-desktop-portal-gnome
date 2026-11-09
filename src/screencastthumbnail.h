/*
 * Copyright © 2026 The xdg-desktop-portal-gnome authors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include <gtk/gtk.h>
#include <stdint.h>

/* Monitor thumbnails: a screenshot of a stage area via org.gnome.Shell.Screenshot */

void         screen_cast_capture_area_async  (const graphene_rect_t *area,
                                              GCancellable          *cancellable,
                                              GAsyncReadyCallback    callback,
                                              gpointer               user_data);

GdkTexture * screen_cast_capture_area_finish (GAsyncResult  *result,
                                              GError       **error);

/* Window thumbnails: the first frame of a Mutter window screen cast */

#define SCREEN_CAST_TYPE_THUMBNAILER (screen_cast_thumbnailer_get_type ())
G_DECLARE_FINAL_TYPE (ScreenCastThumbnailer, screen_cast_thumbnailer,
                      SCREEN_CAST, THUMBNAILER, GObject)

ScreenCastThumbnailer * screen_cast_thumbnailer_new (void);

void         screen_cast_thumbnailer_start   (ScreenCastThumbnailer *self,
                                              GListModel            *windows,
                                              GskRenderer           *renderer);

void         screen_cast_thumbnailer_stop    (ScreenCastThumbnailer *self);

gboolean     screen_cast_thumbnailer_is_started (ScreenCastThumbnailer *self);

gboolean     screen_cast_thumbnailer_is_done (ScreenCastThumbnailer *self);

GdkTexture * screen_cast_thumbnailer_lookup  (ScreenCastThumbnailer *self,
                                              uint64_t               window_id);
