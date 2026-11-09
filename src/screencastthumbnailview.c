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

#include "screencastthumbnailview.h"

/**
 * ScreenCastThumbnailView:
 *
 * Draws a texture scaled to whatever size the widget is allocated.
 *
 * Unlike `GtkPicture`, it requests no size of its own: a thumbnail of a
 * 4K monitor must not make the picker request a 4K natural size (the
 * geometry container derives its natural size from its children).
 */

struct _ScreenCastThumbnailView
{
  GtkWidget parent_instance;

  GdkTexture *texture;
  GtkContentFit content_fit;
};

G_DEFINE_FINAL_TYPE (ScreenCastThumbnailView, screen_cast_thumbnail_view, GTK_TYPE_WIDGET)

static void
screen_cast_thumbnail_view_snapshot (GtkWidget   *widget,
                                     GtkSnapshot *snapshot)
{
  ScreenCastThumbnailView *self = SCREEN_CAST_THUMBNAIL_VIEW (widget);
  int width, height, texture_width, texture_height;
  double scale, draw_width, draw_height;

  if (!self->texture)
    return;

  width = gtk_widget_get_width (widget);
  height = gtk_widget_get_height (widget);
  texture_width = gdk_texture_get_width (self->texture);
  texture_height = gdk_texture_get_height (self->texture);

  if (width <= 0 || height <= 0 || texture_width <= 0 || texture_height <= 0)
    return;

  if (self->content_fit == GTK_CONTENT_FIT_COVER)
    scale = MAX ((double) width / texture_width, (double) height / texture_height);
  else
    scale = MIN ((double) width / texture_width, (double) height / texture_height);

  draw_width = texture_width * scale;
  draw_height = texture_height * scale;

  gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0, 0, width, height));
  gtk_snapshot_append_scaled_texture (snapshot,
                                      self->texture,
                                      GSK_SCALING_FILTER_TRILINEAR,
                                      &GRAPHENE_RECT_INIT ((width - draw_width) / 2.0,
                                                           (height - draw_height) / 2.0,
                                                           draw_width,
                                                           draw_height));
  gtk_snapshot_pop (snapshot);
}

static void
screen_cast_thumbnail_view_measure (GtkWidget      *widget,
                                    GtkOrientation  orientation,
                                    int             for_size,
                                    int            *minimum,
                                    int            *natural,
                                    int            *minimum_baseline,
                                    int            *natural_baseline)
{
  *minimum = 0;
  *natural = 0;
}

static void
screen_cast_thumbnail_view_dispose (GObject *object)
{
  ScreenCastThumbnailView *self = SCREEN_CAST_THUMBNAIL_VIEW (object);

  g_clear_object (&self->texture);

  G_OBJECT_CLASS (screen_cast_thumbnail_view_parent_class)->dispose (object);
}

static void
screen_cast_thumbnail_view_class_init (ScreenCastThumbnailViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = screen_cast_thumbnail_view_dispose;
  widget_class->snapshot = screen_cast_thumbnail_view_snapshot;
  widget_class->measure = screen_cast_thumbnail_view_measure;

  gtk_widget_class_set_css_name (widget_class, "thumbnail");
}

static void
screen_cast_thumbnail_view_init (ScreenCastThumbnailView *self)
{
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

GtkWidget *
screen_cast_thumbnail_view_new (GtkContentFit content_fit)
{
  ScreenCastThumbnailView *self;

  self = g_object_new (SCREEN_CAST_TYPE_THUMBNAIL_VIEW, NULL);
  self->content_fit = content_fit;

  return GTK_WIDGET (self);
}

void
screen_cast_thumbnail_view_set_texture (ScreenCastThumbnailView *self,
                                        GdkTexture              *texture)
{
  g_return_if_fail (SCREEN_CAST_IS_THUMBNAIL_VIEW (self));

  if (g_set_object (&self->texture, texture))
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

GdkTexture *
screen_cast_thumbnail_view_get_texture (ScreenCastThumbnailView *self)
{
  g_return_val_if_fail (SCREEN_CAST_IS_THUMBNAIL_VIEW (self), NULL);

  return self->texture;
}
