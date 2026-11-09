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

#define SCREEN_CAST_TYPE_THUMBNAIL_VIEW (screen_cast_thumbnail_view_get_type ())
G_DECLARE_FINAL_TYPE (ScreenCastThumbnailView, screen_cast_thumbnail_view,
                      SCREEN_CAST, THUMBNAIL_VIEW, GtkWidget)

GtkWidget * screen_cast_thumbnail_view_new (GtkContentFit content_fit);

void screen_cast_thumbnail_view_set_texture (ScreenCastThumbnailView *self,
                                             GdkTexture              *texture);

GdkTexture * screen_cast_thumbnail_view_get_texture (ScreenCastThumbnailView *self);
