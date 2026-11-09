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

#include "config.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <math.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#include "screencastthumbnail.h"
#include "shell-dbus.h"
#include "shellintrospect.h"

/* With shared memory buffers Mutter reads each window frame back from the GPU
 * synchronously on its main thread, stalling the whole compositor; recording
 * every window at once froze the desktop for seconds. Mutter only renders a
 * stream while a consumer streams from it, so we bound that by connecting to
 * a few windows at a time. */
#define MAX_ACTIVE_WINDOW_STREAMS 2

/* How long one window stream gets to deliver its first frame */
#define WINDOW_STREAM_TIMEOUT_MS 1000

/* Covers streams Mutter never announces a PipeWire node for */
#define SESSION_EXTRA_TIMEOUT_MS 1500

/*
 * Monitor thumbnails
 */

typedef struct
{
  int x;
  int y;
  int width;
  int height;
  char *path;
} CaptureAreaData;

/* GNOME Shell refuses a second screenshot request from a sender while one is
 * in progress, so requests are queued and sent one at a time. */
static GQueue capture_queue = G_QUEUE_INIT;
static gboolean capture_in_progress = FALSE;
static OrgGnomeShellScreenshot *shell_screenshot = NULL;

static void capture_next_area (void);

static void
capture_area_data_free (CaptureAreaData *data)
{
  if (data->path)
    g_unlink (data->path);
  g_free (data->path);
  g_free (data);
}

static void
load_texture_in_thread (GTask        *task,
                        gpointer      source_object,
                        gpointer      task_data,
                        GCancellable *cancellable)
{
  CaptureAreaData *data = task_data;
  g_autoptr(GError) error = NULL;
  GdkTexture *texture;

  texture = gdk_texture_new_from_filename (data->path, &error);
  if (!texture)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_return_pointer (task, texture, g_object_unref);
}

static void
on_screenshot_area_finished (GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  g_autoptr(GError) error = NULL;
  g_autofree char *filename_used = NULL;
  gboolean success;

  capture_in_progress = FALSE;
  capture_next_area ();

  if (!org_gnome_shell_screenshot_call_screenshot_area_finish (ORG_GNOME_SHELL_SCREENSHOT (source_object),
                                                               &success,
                                                               &filename_used,
                                                               result,
                                                               &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  if (!success)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "GNOME Shell failed to capture the area");
      return;
    }

  g_task_run_in_thread (task, load_texture_in_thread);
}

static void
send_screenshot_area (GTask *task)
{
  CaptureAreaData *data = g_task_get_task_data (task);

  org_gnome_shell_screenshot_call_screenshot_area (shell_screenshot,
                                                   data->x,
                                                   data->y,
                                                   data->width,
                                                   data->height,
                                                   FALSE,
                                                   data->path,
                                                   g_task_get_cancellable (task),
                                                   on_screenshot_area_finished,
                                                   task);
}

static void
on_shell_screenshot_proxy_ready (GObject      *source_object,
                                 GAsyncResult *result,
                                 gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  g_autoptr(GError) error = NULL;

  shell_screenshot = org_gnome_shell_screenshot_proxy_new_for_bus_finish (result, &error);
  if (!shell_screenshot)
    {
      capture_in_progress = FALSE;
      capture_next_area ();
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  send_screenshot_area (g_steal_pointer (&task));
}

static void
capture_next_area (void)
{
  GTask *task;

  while (!capture_in_progress && (task = g_queue_pop_head (&capture_queue)))
    {
      if (g_task_return_error_if_cancelled (task))
        {
          g_object_unref (task);
          continue;
        }

      capture_in_progress = TRUE;

      if (shell_screenshot)
        {
          send_screenshot_area (task);
        }
      else
        {
          org_gnome_shell_screenshot_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                                        G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                        "org.gnome.Shell.Screenshot",
                                                        "/org/gnome/Shell/Screenshot",
                                                        NULL,
                                                        on_shell_screenshot_proxy_ready,
                                                        task);
        }
    }
}

void
screen_cast_capture_area_async (const graphene_rect_t *area,
                                GCancellable          *cancellable,
                                GAsyncReadyCallback    callback,
                                gpointer               user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;
  CaptureAreaData *data;
  int fd;

  task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, screen_cast_capture_area_async);

  data = g_new0 (CaptureAreaData, 1);
  data->x = round (area->origin.x);
  data->y = round (area->origin.y);
  data->width = round (area->size.width);
  data->height = round (area->size.height);
  g_task_set_task_data (task, data, (GDestroyNotify) capture_area_data_free);

  /* GNOME Shell overwrites the file; creating it first reserves a unique name */
  data->path = g_build_filename (g_get_user_runtime_dir (),
                                 "xdg-desktop-portal-gnome-thumbnail-XXXXXX.png",
                                 NULL);
  fd = g_mkstemp (data->path);
  if (fd == -1)
    {
      int saved_errno = errno;

      g_clear_pointer (&data->path, g_free);
      g_task_return_new_error (task, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                               "Failed to create thumbnail file: %s",
                               g_strerror (saved_errno));
      return;
    }
  g_close (fd, NULL);

  g_queue_push_tail (&capture_queue, g_steal_pointer (&task));
  capture_next_area ();
}

GdkTexture *
screen_cast_capture_area_finish (GAsyncResult  *result,
                                 GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/*
 * Window thumbnails
 */

enum
{
  THUMBNAIL_READY,
  DONE,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef struct
{
  ScreenCastThumbnailer *thumbnailer;
  uint64_t window_id;

  OrgGnomeMutterScreenCastStream *proxy;
  uint32_t node_id;
  unsigned int timeout_id;
  gboolean finished;

  /* Owned by the PipeWire thread while the thread loop runs */
  struct pw_stream *pw_stream;
  struct spa_hook pw_stream_listener;
  struct spa_video_info_raw format;
  gboolean has_format;
  gboolean captured;
} ThumbnailStream;

typedef struct
{
  ScreenCastThumbnailer *thumbnailer;
  uint64_t window_id;
  GBytes *bytes;
  int width;
  int height;
  GdkMemoryFormat format;
} ThumbnailFrame;

struct _ScreenCastThumbnailer
{
  GObject parent_instance;

  OrgGnomeMutterScreenCastSession *session;
  GPtrArray *streams;
  GQueue streams_to_connect;
  GHashTable *textures;

  struct pw_thread_loop *pw_loop;
  struct pw_context *pw_context;
  struct pw_core *pw_core;

  unsigned int n_pending;
  unsigned int n_active;
  unsigned int timeout_id;
  int64_t start_time;
  gboolean started;
  gboolean done;
};

G_DEFINE_FINAL_TYPE (ScreenCastThumbnailer, screen_cast_thumbnailer, G_TYPE_OBJECT)

static gboolean
gdk_format_from_spa_format (uint32_t         spa_format,
                            GdkMemoryFormat *gdk_format)
{
  GdkMemoryFormat format;

  switch (spa_format)
    {
    case SPA_VIDEO_FORMAT_BGRx:
      format = GDK_MEMORY_B8G8R8X8;
      break;
    case SPA_VIDEO_FORMAT_BGRA:
      format = GDK_MEMORY_B8G8R8A8_PREMULTIPLIED;
      break;
    case SPA_VIDEO_FORMAT_RGBx:
      format = GDK_MEMORY_R8G8B8X8;
      break;
    case SPA_VIDEO_FORMAT_RGBA:
      format = GDK_MEMORY_R8G8B8A8_PREMULTIPLIED;
      break;
    default:
      return FALSE;
    }

  if (gdk_format)
    *gdk_format = format;
  return TRUE;
}

static void
thumbnail_frame_free (ThumbnailFrame *frame)
{
  g_clear_object (&frame->thumbnailer);
  g_clear_pointer (&frame->bytes, g_bytes_unref);
  g_free (frame);
}

static void finish_stream (ThumbnailStream *stream);

static gboolean
deliver_frame_cb (gpointer user_data)
{
  ThumbnailFrame *frame = user_data;
  ScreenCastThumbnailer *self = frame->thumbnailer;
  g_autoptr(GdkTexture) texture = NULL;
  unsigned int i;

  texture = gdk_memory_texture_new (frame->width,
                                    frame->height,
                                    frame->format,
                                    frame->bytes,
                                    frame->width * 4);

  g_hash_table_insert (self->textures,
                       g_memdup2 (&frame->window_id, sizeof (frame->window_id)),
                       g_object_ref (texture));
  g_debug ("Window thumbnail %" G_GUINT64_FORMAT " (%dx%d) after %" G_GINT64_FORMAT " ms",
           frame->window_id, frame->width, frame->height,
           (g_get_monotonic_time () - self->start_time) / 1000);
  g_signal_emit (self, signals[THUMBNAIL_READY], 0, frame->window_id, texture);

  /* Streams are gone if the thumbnailer stopped meanwhile */
  for (i = 0; i < self->streams->len; i++)
    {
      ThumbnailStream *stream = g_ptr_array_index (self->streams, i);

      if (stream->window_id == frame->window_id)
        {
          finish_stream (stream);
          break;
        }
    }

  return G_SOURCE_REMOVE;
}

/* Called on the PipeWire thread */
static gboolean
capture_frame (ThumbnailStream    *stream,
               struct spa_buffer  *buffer)
{
  struct spa_meta_header *header;
  struct spa_meta_region *crop;
  struct spa_data *data;
  ThumbnailFrame *frame;
  GdkMemoryFormat gdk_format;
  uint32_t x = 0, y = 0, width, height, stride, row;
  const uint8_t *src;
  uint8_t *pixels;

  header = spa_buffer_find_meta_data (buffer, SPA_META_Header, sizeof (*header));
  if (header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED))
    return FALSE;

  data = &buffer->datas[0];
  if (!data->data || !data->chunk || data->chunk->size == 0 ||
      (data->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED))
    return FALSE;

  if (!gdk_format_from_spa_format (stream->format.format, &gdk_format))
    return FALSE;

  width = stream->format.size.width;
  height = stream->format.size.height;

  crop = spa_buffer_find_meta_data (buffer, SPA_META_VideoCrop, sizeof (*crop));
  if (crop && spa_meta_region_is_valid (crop) &&
      crop->region.position.x >= 0 && crop->region.position.y >= 0 &&
      crop->region.position.x + crop->region.size.width <= width &&
      crop->region.position.y + crop->region.size.height <= height)
    {
      x = crop->region.position.x;
      y = crop->region.position.y;
      width = crop->region.size.width;
      height = crop->region.size.height;
    }

  if (width == 0 || height == 0)
    return FALSE;

  stride = data->chunk->stride > 0 ? (uint32_t) data->chunk->stride
                                   : stream->format.size.width * 4;

  if (data->chunk->offset > data->maxsize ||
      (uint64_t) (y + height - 1) * stride + (uint64_t) (x + width) * 4 >
      data->maxsize - data->chunk->offset)
    return FALSE;

  src = (const uint8_t *) data->data + data->chunk->offset;
  pixels = g_malloc ((gsize) width * height * 4);
  for (row = 0; row < height; row++)
    {
      memcpy (pixels + (gsize) row * width * 4,
              src + (gsize) (y + row) * stride + (gsize) x * 4,
              (gsize) width * 4);
    }

  frame = g_new0 (ThumbnailFrame, 1);
  frame->thumbnailer = g_object_ref (stream->thumbnailer);
  frame->window_id = stream->window_id;
  frame->bytes = g_bytes_new_take (pixels, (gsize) width * height * 4);
  frame->width = width;
  frame->height = height;
  frame->format = gdk_format;

  g_idle_add_full (G_PRIORITY_DEFAULT,
                   deliver_frame_cb,
                   frame,
                   (GDestroyNotify) thumbnail_frame_free);

  return TRUE;
}

/* Called on the PipeWire thread */
static void
on_pw_stream_process (void *user_data)
{
  ThumbnailStream *stream = user_data;
  struct pw_buffer *pw_buffer = NULL;
  struct pw_buffer *next;

  /* Only the newest buffer is interesting */
  while ((next = pw_stream_dequeue_buffer (stream->pw_stream)))
    {
      if (pw_buffer)
        pw_stream_queue_buffer (stream->pw_stream, pw_buffer);
      pw_buffer = next;
    }

  if (!pw_buffer)
    return;

  if (!stream->captured && stream->has_format)
    stream->captured = capture_frame (stream, pw_buffer->buffer);

  pw_stream_queue_buffer (stream->pw_stream, pw_buffer);

  if (stream->captured)
    pw_stream_set_active (stream->pw_stream, false);
}

/* Called on the PipeWire thread */
static void
on_pw_stream_param_changed (void                 *user_data,
                            uint32_t              id,
                            const struct spa_pod *param)
{
  ThumbnailStream *stream = user_data;
  uint8_t params_buffer[1024];
  struct spa_pod_builder builder =
    SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));
  const struct spa_pod *params[3];
  uint32_t media_type, media_subtype;

  if (!param || id != SPA_PARAM_Format)
    return;

  if (spa_format_parse (param, &media_type, &media_subtype) < 0 ||
      media_type != SPA_MEDIA_TYPE_video ||
      media_subtype != SPA_MEDIA_SUBTYPE_raw)
    return;

  if (spa_format_video_raw_parse (param, &stream->format) < 0 ||
      !gdk_format_from_spa_format (stream->format.format, NULL))
    return;

  stream->has_format = TRUE;

  /* Shared memory only: no DMA-BUF, so no GPU readback is needed */
  /* Mutter defaults to 8 window-sized buffers; one frame needs the minimum */
  params[0] = spa_pod_builder_add_object (&builder,
    SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
    SPA_PARAM_BUFFERS_buffers, SPA_POD_Int (2),
    SPA_PARAM_BUFFERS_dataType, SPA_POD_Int ((1 << SPA_DATA_MemPtr) |
                                             (1 << SPA_DATA_MemFd)));
  params[1] = spa_pod_builder_add_object (&builder,
    SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
    SPA_PARAM_META_type, SPA_POD_Id (SPA_META_Header),
    SPA_PARAM_META_size, SPA_POD_Int (sizeof (struct spa_meta_header)));
  params[2] = spa_pod_builder_add_object (&builder,
    SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
    SPA_PARAM_META_type, SPA_POD_Id (SPA_META_VideoCrop),
    SPA_PARAM_META_size, SPA_POD_Int (sizeof (struct spa_meta_region)));

  pw_stream_update_params (stream->pw_stream, params, G_N_ELEMENTS (params));
}

static const struct pw_stream_events pw_stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .param_changed = on_pw_stream_param_changed,
  .process = on_pw_stream_process,
};

static void
connect_pw_stream (ThumbnailStream *stream)
{
  ScreenCastThumbnailer *self = stream->thumbnailer;
  uint32_t node_id = stream->node_id;
  uint8_t params_buffer[1024];
  struct spa_pod_builder builder =
    SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));
  const struct spa_pod *params[1];

  params[0] = spa_pod_builder_add_object (&builder,
    SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
    SPA_FORMAT_mediaType, SPA_POD_Id (SPA_MEDIA_TYPE_video),
    SPA_FORMAT_mediaSubtype, SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw),
    SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id (5,
                                                     SPA_VIDEO_FORMAT_BGRx,
                                                     SPA_VIDEO_FORMAT_BGRx,
                                                     SPA_VIDEO_FORMAT_BGRA,
                                                     SPA_VIDEO_FORMAT_RGBx,
                                                     SPA_VIDEO_FORMAT_RGBA),
    SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle (&SPA_RECTANGLE (320, 240),
                                                           &SPA_RECTANGLE (1, 1),
                                                           &SPA_RECTANGLE (16384, 16384)),
    SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction (&SPA_FRACTION (0, 1),
                                                               &SPA_FRACTION (0, 1),
                                                               &SPA_FRACTION (360, 1)));

  pw_thread_loop_lock (self->pw_loop);

  stream->pw_stream =
    pw_stream_new (self->pw_core,
                   "xdg-desktop-portal-gnome window thumbnail",
                   pw_properties_new (PW_KEY_MEDIA_TYPE, "Video",
                                      PW_KEY_MEDIA_CATEGORY, "Capture",
                                      PW_KEY_MEDIA_ROLE, "Screen",
                                      NULL));
  if (stream->pw_stream)
    {
      pw_stream_add_listener (stream->pw_stream,
                              &stream->pw_stream_listener,
                              &pw_stream_events,
                              stream);

      if (pw_stream_connect (stream->pw_stream,
                             PW_DIRECTION_INPUT,
                             node_id,
                             PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                             params, G_N_ELEMENTS (params)) < 0)
        g_debug ("Failed to connect to PipeWire node %u", node_id);
    }

  pw_thread_loop_unlock (self->pw_loop);
}

static void
disconnect_pw_stream (ThumbnailStream *stream)
{
  ScreenCastThumbnailer *self = stream->thumbnailer;

  if (!stream->pw_stream)
    return;

  pw_thread_loop_lock (self->pw_loop);
  spa_hook_remove (&stream->pw_stream_listener);
  g_clear_pointer (&stream->pw_stream, pw_stream_destroy);
  pw_thread_loop_unlock (self->pw_loop);
}

static gboolean
on_stream_timeout_cb (gpointer user_data)
{
  ThumbnailStream *stream = user_data;

  stream->timeout_id = 0;
  g_debug ("Window thumbnail %" G_GUINT64_FORMAT " timed out", stream->window_id);
  finish_stream (stream);

  return G_SOURCE_REMOVE;
}

static void
connect_more_streams (ScreenCastThumbnailer *self)
{
  ThumbnailStream *stream;

  while (self->n_active < MAX_ACTIVE_WINDOW_STREAMS &&
         (stream = g_queue_pop_head (&self->streams_to_connect)))
    {
      connect_pw_stream (stream);
      self->n_active++;
      stream->timeout_id = g_timeout_add (WINDOW_STREAM_TIMEOUT_MS,
                                          on_stream_timeout_cb,
                                          stream);
    }
}

/* The stream got its frame or gave up; free its slot for the next one */
static void
finish_stream (ThumbnailStream *stream)
{
  ScreenCastThumbnailer *self = stream->thumbnailer;

  if (stream->finished)
    return;

  stream->finished = TRUE;
  g_clear_handle_id (&stream->timeout_id, g_source_remove);
  disconnect_pw_stream (stream);

  if (self->n_active > 0)
    self->n_active--;

  if (self->n_pending > 0 && --self->n_pending == 0)
    screen_cast_thumbnailer_stop (self);
  else
    connect_more_streams (self);
}

static void
on_pipewire_stream_added (OrgGnomeMutterScreenCastStream *proxy,
                          unsigned int                    node_id,
                          ThumbnailStream                *stream)
{
  ScreenCastThumbnailer *self = stream->thumbnailer;

  if (self->done || !self->pw_core || stream->finished || stream->node_id != SPA_ID_INVALID)
    return;

  stream->node_id = node_id;
  g_queue_push_tail (&self->streams_to_connect, stream);
  connect_more_streams (self);
}

static void
thumbnail_stream_free (ThumbnailStream *stream)
{
  /* PipeWire objects are torn down before streams are freed */
  g_assert (stream->pw_stream == NULL);

  g_clear_handle_id (&stream->timeout_id, g_source_remove);

  if (stream->proxy)
    {
      g_signal_handlers_disconnect_by_data (stream->proxy, stream);
      g_clear_object (&stream->proxy);
    }
  g_free (stream);
}

static gboolean
setup_pipewire (ScreenCastThumbnailer  *self,
                GError                **error)
{
  static gsize pw_initialized = 0;

  if (g_once_init_enter (&pw_initialized))
    {
      pw_init (NULL, NULL);
      g_once_init_leave (&pw_initialized, 1);
    }

  self->pw_loop = pw_thread_loop_new ("xdpg-thumbnails", NULL);
  if (!self->pw_loop)
    goto fail;

  self->pw_context = pw_context_new (pw_thread_loop_get_loop (self->pw_loop), NULL, 0);
  if (!self->pw_context)
    goto fail;

  if (pw_thread_loop_start (self->pw_loop) < 0)
    goto fail;

  pw_thread_loop_lock (self->pw_loop);
  self->pw_core = pw_context_connect (self->pw_context, NULL, 0);
  pw_thread_loop_unlock (self->pw_loop);

  if (!self->pw_core)
    goto fail;

  return TRUE;

fail:
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "Failed to connect to PipeWire: %s", g_strerror (errno));
  return FALSE;
}

static void
teardown_pipewire (ScreenCastThumbnailer *self)
{
  unsigned int i;

  if (!self->pw_loop)
    return;

  pw_thread_loop_lock (self->pw_loop);

  for (i = 0; i < self->streams->len; i++)
    {
      ThumbnailStream *stream = g_ptr_array_index (self->streams, i);

      if (stream->pw_stream)
        {
          spa_hook_remove (&stream->pw_stream_listener);
          g_clear_pointer (&stream->pw_stream, pw_stream_destroy);
        }
    }

  g_clear_pointer (&self->pw_core, pw_core_disconnect);

  pw_thread_loop_unlock (self->pw_loop);
  pw_thread_loop_stop (self->pw_loop);

  g_clear_pointer (&self->pw_context, pw_context_destroy);
  g_clear_pointer (&self->pw_loop, pw_thread_loop_destroy);
}

static gboolean
record_window (ScreenCastThumbnailer  *self,
               GDBusConnection        *connection,
               uint64_t                window_id,
               GError                **error)
{
  GVariantBuilder properties_builder;
  g_autofree char *stream_path = NULL;
  ThumbnailStream *stream;

  g_variant_builder_init (&properties_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&properties_builder, "{sv}",
                         "window-id", g_variant_new_uint64 (window_id));

  if (!org_gnome_mutter_screen_cast_session_call_record_window_sync (self->session,
                                                                     g_variant_builder_end (&properties_builder),
                                                                     &stream_path,
                                                                     NULL,
                                                                     error))
    return FALSE;

  stream = g_new0 (ThumbnailStream, 1);
  stream->thumbnailer = self;
  stream->window_id = window_id;
  stream->node_id = SPA_ID_INVALID;
  stream->proxy =
    org_gnome_mutter_screen_cast_stream_proxy_new_sync (connection,
                                                        G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                        "org.gnome.Mutter.ScreenCast",
                                                        stream_path,
                                                        NULL,
                                                        error);
  if (!stream->proxy)
    {
      thumbnail_stream_free (stream);
      return FALSE;
    }

  g_signal_connect (stream->proxy, "pipewire-stream-added",
                    G_CALLBACK (on_pipewire_stream_added),
                    stream);

  g_ptr_array_add (self->streams, stream);
  return TRUE;
}

static gboolean
on_timeout_cb (gpointer user_data)
{
  ScreenCastThumbnailer *self = user_data;

  self->timeout_id = 0;
  g_debug ("%u window thumbnails still pending, giving up", self->n_pending);
  screen_cast_thumbnailer_stop (self);

  return G_SOURCE_REMOVE;
}

static gboolean
start_session (ScreenCastThumbnailer  *self,
               GListModel             *windows,
               GError                **error)
{
  g_autoptr(GDBusConnection) connection = NULL;
  g_autoptr(OrgGnomeMutterScreenCast) screen_cast = NULL;
  GVariantBuilder properties_builder;
  g_autofree char *session_path = NULL;
  unsigned int i;

  if (g_list_model_get_n_items (windows) == 0)
    return TRUE;

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (!connection)
    return FALSE;

  screen_cast =
    org_gnome_mutter_screen_cast_proxy_new_sync (connection,
                                                 G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                 "org.gnome.Mutter.ScreenCast",
                                                 "/org/gnome/Mutter/ScreenCast",
                                                 NULL,
                                                 error);
  if (!screen_cast)
    return FALSE;

  g_variant_builder_init (&properties_builder, G_VARIANT_TYPE_VARDICT);
  if (!org_gnome_mutter_screen_cast_call_create_session_sync (screen_cast,
                                                              g_variant_builder_end (&properties_builder),
                                                              &session_path,
                                                              NULL,
                                                              error))
    return FALSE;

  self->session =
    org_gnome_mutter_screen_cast_session_proxy_new_sync (connection,
                                                         G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                         "org.gnome.Mutter.ScreenCast",
                                                         session_path,
                                                         NULL,
                                                         error);
  if (!self->session)
    return FALSE;

  if (!setup_pipewire (self, error))
    return FALSE;

  for (i = 0; i < g_list_model_get_n_items (windows); i++)
    {
      g_autoptr(ShellWindow) window = g_list_model_get_item (windows, i);
      g_autoptr(GError) local_error = NULL;

      if (!record_window (self, connection, shell_window_get_id (window), &local_error))
        g_debug ("Not thumbnailing window %s: %s",
                 shell_window_get_title (window), local_error->message);
    }

  self->n_pending = self->streams->len;
  if (self->n_pending == 0)
    return TRUE;

  if (!org_gnome_mutter_screen_cast_session_call_start_sync (self->session, NULL, error))
    return FALSE;

  self->start_time = g_get_monotonic_time ();
  self->timeout_id =
    g_timeout_add (SESSION_EXTRA_TIMEOUT_MS +
                   WINDOW_STREAM_TIMEOUT_MS * ((self->n_pending + MAX_ACTIVE_WINDOW_STREAMS - 1) /
                                               MAX_ACTIVE_WINDOW_STREAMS),
                   on_timeout_cb, self);
  return TRUE;
}

static void
release_session (ScreenCastThumbnailer *self)
{
  g_clear_handle_id (&self->timeout_id, g_source_remove);

  teardown_pipewire (self);
  g_queue_clear (&self->streams_to_connect);
  g_ptr_array_set_size (self->streams, 0);

  if (self->session)
    {
      org_gnome_mutter_screen_cast_session_call_stop (self->session, NULL, NULL, NULL);
      g_clear_object (&self->session);
    }

  self->n_pending = 0;
  self->n_active = 0;
}

/*
 * GObject overrides
 */

static void
screen_cast_thumbnailer_dispose (GObject *object)
{
  ScreenCastThumbnailer *self = SCREEN_CAST_THUMBNAILER (object);

  release_session (self);

  G_OBJECT_CLASS (screen_cast_thumbnailer_parent_class)->dispose (object);
}

static void
screen_cast_thumbnailer_finalize (GObject *object)
{
  ScreenCastThumbnailer *self = SCREEN_CAST_THUMBNAILER (object);

  g_clear_pointer (&self->streams, g_ptr_array_unref);
  g_clear_pointer (&self->textures, g_hash_table_unref);

  G_OBJECT_CLASS (screen_cast_thumbnailer_parent_class)->finalize (object);
}

static void
screen_cast_thumbnailer_class_init (ScreenCastThumbnailerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = screen_cast_thumbnailer_dispose;
  object_class->finalize = screen_cast_thumbnailer_finalize;

  signals[THUMBNAIL_READY] = g_signal_new ("thumbnail-ready",
                                           G_TYPE_FROM_CLASS (klass),
                                           G_SIGNAL_RUN_LAST,
                                           0,
                                           NULL, NULL,
                                           NULL,
                                           G_TYPE_NONE, 2,
                                           G_TYPE_UINT64,
                                           GDK_TYPE_TEXTURE);
  signals[DONE] = g_signal_new ("done",
                                G_TYPE_FROM_CLASS (klass),
                                G_SIGNAL_RUN_LAST,
                                0,
                                NULL, NULL,
                                NULL,
                                G_TYPE_NONE, 0);
}

static void
screen_cast_thumbnailer_init (ScreenCastThumbnailer *self)
{
  self->streams = g_ptr_array_new_with_free_func ((GDestroyNotify) thumbnail_stream_free);
  self->textures = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                          g_free, g_object_unref);
}

ScreenCastThumbnailer *
screen_cast_thumbnailer_new (void)
{
  return g_object_new (SCREEN_CAST_TYPE_THUMBNAILER, NULL);
}

/**
 * screen_cast_thumbnailer_start:
 * @windows: a list of `ShellWindow`s
 *
 * Records each window once and emits `thumbnail-ready` per captured frame,
 * then `done` when all frames arrived, on timeout, or on failure. Can only be
 * started once.
 */
void
screen_cast_thumbnailer_start (ScreenCastThumbnailer *self,
                               GListModel            *windows)
{
  g_autoptr(GError) error = NULL;

  g_return_if_fail (SCREEN_CAST_IS_THUMBNAILER (self));

  if (self->started)
    return;

  self->started = TRUE;

  if (!start_session (self, windows, &error))
    g_warning ("Failed to capture window thumbnails: %s", error->message);

  if (self->n_pending == 0)
    screen_cast_thumbnailer_stop (self);
}

void
screen_cast_thumbnailer_stop (ScreenCastThumbnailer *self)
{
  g_return_if_fail (SCREEN_CAST_IS_THUMBNAILER (self));

  if (self->done)
    return;

  self->done = TRUE;
  if (self->start_time)
    g_debug ("Window thumbnails done after %" G_GINT64_FORMAT " ms",
             (g_get_monotonic_time () - self->start_time) / 1000);
  release_session (self);

  if (self->started)
    g_signal_emit (self, signals[DONE], 0);
}

gboolean
screen_cast_thumbnailer_is_started (ScreenCastThumbnailer *self)
{
  return self->started;
}

gboolean
screen_cast_thumbnailer_is_done (ScreenCastThumbnailer *self)
{
  return self->done;
}

GdkTexture *
screen_cast_thumbnailer_lookup (ScreenCastThumbnailer *self,
                                uint64_t               window_id)
{
  return g_hash_table_lookup (self->textures, &window_id);
}
