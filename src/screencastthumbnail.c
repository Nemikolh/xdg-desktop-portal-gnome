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

#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
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
 * every window at once froze the desktop for seconds. DMA-BUFs avoid the
 * readback, but shared memory stays the fallback. Mutter only renders a
 * stream while a consumer streams from it, so we bound that by connecting to
 * a few windows at a time. */
#define MAX_ACTIVE_WINDOW_STREAMS 2

/* Mutter re-records a window on every damage, up to the negotiated maximum
 * frame rate, until the stream pauses; one frame is all we need */
#define WINDOW_STREAM_MAX_FRAMERATE 5

/* Thumbnails are kept at most this large, in pixels; cards are much smaller,
 * so this leaves room for scaled displays */
#define THUMBNAIL_MAX_WIDTH 480
#define THUMBNAIL_MAX_HEIGHT 320

#define MAX_DMABUF_PLANES 4

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

  int64_t queue_time;
  int64_t send_time;
  int64_t shell_done_time;
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

  g_debug ("Monitor thumbnail %dx%d+%d+%d (%dx%d): waited %" G_GINT64_FORMAT " ms, "
           "GNOME Shell took %" G_GINT64_FORMAT " ms, decoding %" G_GINT64_FORMAT " ms",
           data->width, data->height, data->x, data->y,
           gdk_texture_get_width (texture), gdk_texture_get_height (texture),
           (data->send_time - data->queue_time) / 1000,
           (data->shell_done_time - data->send_time) / 1000,
           (g_get_monotonic_time () - data->shell_done_time) / 1000);

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
  CaptureAreaData *data = g_task_get_task_data (task);
  gboolean success;

  data->shell_done_time = g_get_monotonic_time ();
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

  data->send_time = g_get_monotonic_time ();
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
  data->queue_time = g_get_monotonic_time ();
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
  gboolean dmabuf;
  gboolean captured;
} ThumbnailStream;

typedef struct
{
  uint32_t fourcc;
  uint64_t modifier;
  unsigned int width;
  unsigned int height;
  unsigned int n_planes;
  int fds[MAX_DMABUF_PLANES];
  uint32_t offsets[MAX_DMABUF_PLANES];
  uint32_t strides[MAX_DMABUF_PLANES];
} ThumbnailDmabuf;

typedef struct
{
  ScreenCastThumbnailer *thumbnailer;
  uint64_t window_id;
  int64_t capture_time;

  /* Visible area of the frame */
  int x;
  int y;
  int width;
  int height;

  /* Shared memory frames are already cropped to the visible area */
  GBytes *bytes;
  GdkMemoryFormat format;

  /* DMA-BUF frames hold their own file descriptors */
  ThumbnailDmabuf *dmabuf;
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

  GskRenderer *renderer;
  gboolean dmabuf_failed;

  /* Windows still to record, and RecordWindow calls in flight */
  GPtrArray *windows;
  unsigned int n_recording;

  unsigned int n_pending;
  unsigned int n_active;
  unsigned int timeout_id;
  int64_t setup_start_time;
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

static gboolean
drm_fourcc_from_spa_format (uint32_t  spa_format,
                            uint32_t *fourcc)
{
  switch (spa_format)
    {
    case SPA_VIDEO_FORMAT_BGRx:
      *fourcc = DRM_FORMAT_XRGB8888;
      return TRUE;
    case SPA_VIDEO_FORMAT_BGRA:
      *fourcc = DRM_FORMAT_ARGB8888;
      return TRUE;
    case SPA_VIDEO_FORMAT_RGBx:
      *fourcc = DRM_FORMAT_XBGR8888;
      return TRUE;
    case SPA_VIDEO_FORMAT_RGBA:
      *fourcc = DRM_FORMAT_ABGR8888;
      return TRUE;
    default:
      return FALSE;
    }
}

static void
thumbnail_dmabuf_free (ThumbnailDmabuf *dmabuf)
{
  unsigned int i;

  for (i = 0; i < dmabuf->n_planes; i++)
    g_clear_fd (&dmabuf->fds[i], NULL);
  g_free (dmabuf);
}

static void
thumbnail_frame_free (ThumbnailFrame *frame)
{
  g_clear_object (&frame->thumbnailer);
  g_clear_pointer (&frame->bytes, g_bytes_unref);
  g_clear_pointer (&frame->dmabuf, thumbnail_dmabuf_free);
  g_free (frame);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ThumbnailFrame, thumbnail_frame_free)

static void finish_stream (ThumbnailStream *stream);
static void retry_stream (ThumbnailStream *stream);

static GdkTexture *
import_dmabuf (ThumbnailDmabuf  *dmabuf,
               GError          **error)
{
  g_autoptr(GdkDmabufTextureBuilder) builder = NULL;
  GdkTexture *texture;
  unsigned int i;

  builder = gdk_dmabuf_texture_builder_new ();
  gdk_dmabuf_texture_builder_set_display (builder, gdk_display_get_default ());
  gdk_dmabuf_texture_builder_set_width (builder, dmabuf->width);
  gdk_dmabuf_texture_builder_set_height (builder, dmabuf->height);
  gdk_dmabuf_texture_builder_set_fourcc (builder, dmabuf->fourcc);
  gdk_dmabuf_texture_builder_set_modifier (builder, dmabuf->modifier);
  gdk_dmabuf_texture_builder_set_premultiplied (builder, TRUE);
  gdk_dmabuf_texture_builder_set_n_planes (builder, dmabuf->n_planes);
  for (i = 0; i < dmabuf->n_planes; i++)
    {
      gdk_dmabuf_texture_builder_set_fd (builder, i, dmabuf->fds[i]);
      gdk_dmabuf_texture_builder_set_offset (builder, i, dmabuf->offsets[i]);
      gdk_dmabuf_texture_builder_set_stride (builder, i, dmabuf->strides[i]);
    }

  /* On success the texture owns the file descriptors */
  texture = gdk_dmabuf_texture_builder_build (builder,
                                              (GDestroyNotify) thumbnail_dmabuf_free,
                                              dmabuf,
                                              error);
  return texture;
}

/* Renders the visible @area of @texture into a small texture, so neither the
 * full-size frame nor its DMA-BUF have to be kept around for the dialog. */
static GdkTexture *
scale_down_texture (ScreenCastThumbnailer *self,
                    GdkTexture            *texture,
                    int                    x,
                    int                    y,
                    int                    width,
                    int                    height)
{
  g_autoptr(GskRenderNode) node = NULL;
  GdkTexture *scaled;
  int texture_width = gdk_texture_get_width (texture);
  int texture_height = gdk_texture_get_height (texture);
  gboolean cropped;
  double scale;
  int scaled_width, scaled_height;

  cropped = x != 0 || y != 0 || width != texture_width || height != texture_height;
  scale = MIN (1.0, MIN ((double) THUMBNAIL_MAX_WIDTH / width,
                         (double) THUMBNAIL_MAX_HEIGHT / height));

  if (!self->renderer || !gsk_renderer_is_realized (self->renderer))
    return g_object_ref (texture);

  if (scale == 1.0 && !cropped && GDK_IS_MEMORY_TEXTURE (texture))
    return g_object_ref (texture);

  scaled_width = MAX (1, (int) round (width * scale));
  scaled_height = MAX (1, (int) round (height * scale));

  node = gsk_texture_scale_node_new (texture,
                                     &GRAPHENE_RECT_INIT (-x * scale,
                                                          -y * scale,
                                                          texture_width * scale,
                                                          texture_height * scale),
                                     GSK_SCALING_FILTER_TRILINEAR);

  scaled = gsk_renderer_render_texture (self->renderer,
                                        node,
                                        &GRAPHENE_RECT_INIT (0, 0,
                                                             scaled_width,
                                                             scaled_height));
  return scaled ? scaled : g_object_ref (texture);
}

static gboolean
deliver_frame_cb (gpointer user_data)
{
  ThumbnailFrame *frame = user_data;
  ScreenCastThumbnailer *self = frame->thumbnailer;
  g_autoptr(GdkTexture) full_texture = NULL;
  g_autoptr(GdkTexture) texture = NULL;
  g_autoptr(GError) error = NULL;
  int64_t start_time = g_get_monotonic_time ();
  gboolean retry = FALSE;
  unsigned int i;

  if (frame->dmabuf)
    {
      full_texture = import_dmabuf (frame->dmabuf, &error);
      if (full_texture)
        {
          frame->dmabuf = NULL;
        }
      else
        {
          /* Shared memory still works, for this window and the ones left */
          g_debug ("Failed to import window thumbnail DMA-BUF, falling back to shared memory: %s",
                   error->message);
          self->dmabuf_failed = TRUE;
          retry = TRUE;
        }
    }
  else
    {
      full_texture = gdk_memory_texture_new (frame->width,
                                             frame->height,
                                             frame->format,
                                             frame->bytes,
                                             frame->width * 4);
    }

  if (full_texture)
    {
      texture = scale_down_texture (self, full_texture,
                                    frame->x, frame->y,
                                    frame->width, frame->height);
      g_clear_object (&full_texture);

      g_hash_table_insert (self->textures,
                           g_memdup2 (&frame->window_id, sizeof (frame->window_id)),
                           g_object_ref (texture));
      g_debug ("Window thumbnail %" G_GUINT64_FORMAT " (%dx%d, %s, scaled to %dx%d) "
               "after %" G_GINT64_FORMAT " ms, %" G_GINT64_FORMAT " ms from capture, "
               "%" G_GINT64_FORMAT " us to import and scale",
               frame->window_id, frame->width, frame->height,
               frame->bytes ? "shm" : "dmabuf",
               gdk_texture_get_width (texture), gdk_texture_get_height (texture),
               (g_get_monotonic_time () - self->start_time) / 1000,
               (start_time - frame->capture_time) / 1000,
               g_get_monotonic_time () - start_time);
      g_signal_emit (self, signals[THUMBNAIL_READY], 0, frame->window_id, texture);
    }

  /* Streams are gone if the thumbnailer stopped meanwhile */
  for (i = 0; i < self->streams->len; i++)
    {
      ThumbnailStream *stream = g_ptr_array_index (self->streams, i);

      if (stream->window_id == frame->window_id)
        {
          if (retry)
            retry_stream (stream);
          else
            finish_stream (stream);
          break;
        }
    }

  return G_SOURCE_REMOVE;
}

/* Called on the PipeWire thread */
static void
copy_shm_frame (ThumbnailFrame        *frame,
                const struct spa_data *data,
                uint32_t               stride)
{
  const uint8_t *src = (const uint8_t *) data->data + data->chunk->offset;
  uint8_t *pixels;
  int row;

  pixels = g_malloc ((gsize) frame->width * frame->height * 4);
  for (row = 0; row < frame->height; row++)
    {
      memcpy (pixels + (gsize) row * frame->width * 4,
              src + (gsize) (frame->y + row) * stride + (gsize) frame->x * 4,
              (gsize) frame->width * 4);
    }

  frame->bytes = g_bytes_new_take (pixels, (gsize) frame->width * frame->height * 4);
  frame->x = 0;
  frame->y = 0;
}

/* Called on the PipeWire thread */
static ThumbnailDmabuf *
dup_dmabuf (ThumbnailStream   *stream,
            struct spa_buffer *buffer)
{
  ThumbnailDmabuf *dmabuf;
  unsigned int i;

  if (buffer->n_datas == 0 || buffer->n_datas > MAX_DMABUF_PLANES)
    return NULL;

  dmabuf = g_new0 (ThumbnailDmabuf, 1);
  dmabuf->modifier = stream->format.modifier;
  dmabuf->width = stream->format.size.width;
  dmabuf->height = stream->format.size.height;
  if (!drm_fourcc_from_spa_format (stream->format.format, &dmabuf->fourcc))
    goto fail;

  for (i = 0; i < buffer->n_datas; i++)
    {
      const struct spa_data *data = &buffer->datas[i];

      if (data->type != SPA_DATA_DmaBuf || data->fd < 0 || !data->chunk)
        goto fail;

      /* Our own references keep the buffer contents alive after the stream
       * is gone, so the PipeWire buffer can go back to Mutter right away */
      dmabuf->fds[i] = fcntl (data->fd, F_DUPFD_CLOEXEC, 3);
      if (dmabuf->fds[i] < 0)
        goto fail;
      dmabuf->n_planes++;

      dmabuf->offsets[i] = data->chunk->offset;
      dmabuf->strides[i] = data->chunk->stride > 0 ? (uint32_t) data->chunk->stride
                                                    : dmabuf->width * 4;
    }

  return dmabuf;

fail:
  thumbnail_dmabuf_free (dmabuf);
  return NULL;
}

/* Called on the PipeWire thread */
static gboolean
capture_frame (ThumbnailStream    *stream,
               struct spa_buffer  *buffer)
{
  g_autoptr(ThumbnailFrame) frame = NULL;
  struct spa_meta_header *header;
  struct spa_meta_region *crop;
  struct spa_data *data;
  GdkMemoryFormat gdk_format;
  uint32_t x = 0, y = 0, width, height;

  header = spa_buffer_find_meta_data (buffer, SPA_META_Header, sizeof (*header));
  if (header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED))
    return FALSE;

  data = &buffer->datas[0];
  if (!data->chunk || data->chunk->size == 0 ||
      (data->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED))
    return FALSE;

  if (data->type == SPA_DATA_DmaBuf ? !stream->dmabuf : !data->data)
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

  frame = g_new0 (ThumbnailFrame, 1);
  frame->thumbnailer = g_object_ref (stream->thumbnailer);
  frame->window_id = stream->window_id;
  frame->capture_time = g_get_monotonic_time ();
  frame->x = x;
  frame->y = y;
  frame->width = width;
  frame->height = height;
  frame->format = gdk_format;

  if (data->type == SPA_DATA_DmaBuf)
    {
      frame->dmabuf = dup_dmabuf (stream, buffer);
      if (!frame->dmabuf)
        return FALSE;
    }
  else
    {
      uint32_t stride = data->chunk->stride > 0 ? (uint32_t) data->chunk->stride
                                                : stream->format.size.width * 4;

      if (data->chunk->offset > data->maxsize ||
          (uint64_t) (y + height - 1) * stride + (uint64_t) (x + width) * 4 >
          data->maxsize - data->chunk->offset)
        return FALSE;

      copy_shm_frame (frame, data, stride);
    }

  g_idle_add_full (G_PRIORITY_DEFAULT,
                   deliver_frame_cb,
                   g_steal_pointer (&frame),
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
  const struct spa_pod_prop *modifier;
  uint32_t media_type, media_subtype;
  uint32_t data_types;

  if (!param || id != SPA_PARAM_Format)
    return;

  stream->has_format = FALSE;

  if (spa_format_parse (param, &media_type, &media_subtype) < 0 ||
      media_type != SPA_MEDIA_TYPE_video ||
      media_subtype != SPA_MEDIA_SUBTYPE_raw)
    return;

  if (spa_format_video_raw_parse (param, &stream->format) < 0 ||
      !gdk_format_from_spa_format (stream->format.format, NULL))
    return;

  modifier = spa_pod_find_prop (param, NULL, SPA_FORMAT_VIDEO_modifier);

  /* Mutter picks one of the modifiers and renegotiates with it */
  if (modifier && (modifier->flags & SPA_POD_PROP_FLAG_DONT_FIXATE))
    return;

  stream->dmabuf = modifier != NULL;
  stream->has_format = TRUE;

  g_debug ("Window thumbnail %" G_GUINT64_FORMAT ": %ux%u, %s, modifier 0x%" G_GINT64_MODIFIER "x, "
           "max %u/%u fps",
           stream->window_id,
           stream->format.size.width, stream->format.size.height,
           stream->dmabuf ? "dmabuf" : "shm",
           stream->dmabuf ? stream->format.modifier : 0,
           stream->format.max_framerate.num, stream->format.max_framerate.denom);

  /* With shared memory, Mutter reads every frame back from the GPU */
  if (stream->dmabuf)
    data_types = 1 << SPA_DATA_DmaBuf;
  else
    data_types = (1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd);

  /* Mutter defaults to 8 window-sized buffers; one frame needs the minimum */
  params[0] = spa_pod_builder_add_object (&builder,
    SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
    SPA_PARAM_BUFFERS_buffers, SPA_POD_Int (2),
    SPA_PARAM_BUFFERS_dataType, SPA_POD_Int (data_types));
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

static const uint32_t window_stream_formats[] = {
  SPA_VIDEO_FORMAT_BGRx,
  SPA_VIDEO_FORMAT_BGRA,
  SPA_VIDEO_FORMAT_RGBx,
  SPA_VIDEO_FORMAT_RGBA,
};

static void
add_common_format_props (struct spa_pod_builder *builder)
{
  spa_pod_builder_add (builder,
    SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle (&SPA_RECTANGLE (320, 240),
                                                           &SPA_RECTANGLE (1, 1),
                                                           &SPA_RECTANGLE (16384, 16384)),
    SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction (&SPA_FRACTION (0, 1),
                                                               &SPA_FRACTION (0, 1),
                                                               &SPA_FRACTION (360, 1)),
    SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction (&SPA_FRACTION (WINDOW_STREAM_MAX_FRAMERATE, 1),
                                                                  &SPA_FRACTION (1, 1),
                                                                  &SPA_FRACTION (WINDOW_STREAM_MAX_FRAMERATE, 1)),
    0);
}

/* One format with all the modifiers GTK can import it with, so Mutter
 * renders into a DMA-BUF instead of reading the frame back */
static const struct spa_pod *
build_dmabuf_format_param (struct spa_pod_builder *builder,
                           GdkDmabufFormats       *dmabuf_formats,
                           uint32_t                spa_format)
{
  struct spa_pod_frame format_frame, choice_frame;
  gboolean has_modifier = FALSE;
  uint32_t fourcc;
  gsize i;

  if (!drm_fourcc_from_spa_format (spa_format, &fourcc))
    return NULL;

  spa_pod_builder_push_object (builder, &format_frame,
                               SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
  spa_pod_builder_add (builder,
    SPA_FORMAT_mediaType, SPA_POD_Id (SPA_MEDIA_TYPE_video),
    SPA_FORMAT_mediaSubtype, SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw),
    SPA_FORMAT_VIDEO_format, SPA_POD_Id (spa_format),
    0);

  spa_pod_builder_prop (builder, SPA_FORMAT_VIDEO_modifier,
                        SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
  spa_pod_builder_push_choice (builder, &choice_frame, SPA_CHOICE_Enum, 0);
  for (i = 0; i < gdk_dmabuf_formats_get_n_formats (dmabuf_formats); i++)
    {
      uint32_t format_fourcc;
      uint64_t modifier;

      gdk_dmabuf_formats_get_format (dmabuf_formats, i, &format_fourcc, &modifier);
      if (format_fourcc != fourcc || modifier == DRM_FORMAT_MOD_INVALID)
        continue;

      /* The first value of an enum choice is its default */
      if (!has_modifier)
        spa_pod_builder_long (builder, modifier);
      spa_pod_builder_long (builder, modifier);
      has_modifier = TRUE;
    }
  spa_pod_builder_pop (builder, &choice_frame);

  add_common_format_props (builder);

  if (!has_modifier)
    {
      spa_pod_builder_pop (builder, &format_frame);
      return NULL;
    }

  return spa_pod_builder_pop (builder, &format_frame);
}

static void
connect_pw_stream (ThumbnailStream *stream)
{
  ScreenCastThumbnailer *self = stream->thumbnailer;
  uint32_t node_id = stream->node_id;
  uint8_t params_buffer[8192];
  struct spa_pod_builder builder =
    SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));
  struct spa_pod_frame format_frame;
  const struct spa_pod *params[G_N_ELEMENTS (window_stream_formats) + 1];
  const struct spa_pod *param;
  unsigned int n_params = 0;
  unsigned int i;

  /* PipeWire tries formats in order, so DMA-BUFs win when both sides
   * support them; shared memory stays as the fallback */
  if (!self->dmabuf_failed)
    {
      GdkDmabufFormats *dmabuf_formats =
        gdk_display_get_dmabuf_formats (gdk_display_get_default ());

      for (i = 0; dmabuf_formats && i < G_N_ELEMENTS (window_stream_formats); i++)
        {
          param = build_dmabuf_format_param (&builder, dmabuf_formats,
                                             window_stream_formats[i]);
          if (param)
            params[n_params++] = param;
        }
    }

  spa_pod_builder_push_object (&builder, &format_frame,
                               SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
  spa_pod_builder_add (&builder,
    SPA_FORMAT_mediaType, SPA_POD_Id (SPA_MEDIA_TYPE_video),
    SPA_FORMAT_mediaSubtype, SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw),
    SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id (5,
                                                     SPA_VIDEO_FORMAT_BGRx,
                                                     SPA_VIDEO_FORMAT_BGRx,
                                                     SPA_VIDEO_FORMAT_BGRA,
                                                     SPA_VIDEO_FORMAT_RGBx,
                                                     SPA_VIDEO_FORMAT_RGBA),
    0);
  add_common_format_props (&builder);
  param = spa_pod_builder_pop (&builder, &format_frame);
  if (param)
    params[n_params++] = param;

  /* Overflowing the builder leaves earlier params pointing into it */
  if (builder.state.offset > builder.size)
    {
      g_debug ("Window thumbnail format params don't fit, using shared memory");
      self->dmabuf_failed = TRUE;
      connect_pw_stream (stream);
      return;
    }

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
                             params, n_params) < 0)
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

/* Connects again, keeping the slot, now that DMA-BUFs are off */
static void
retry_stream (ThumbnailStream *stream)
{
  if (stream->finished)
    return;

  disconnect_pw_stream (stream);
  stream->has_format = FALSE;
  stream->dmabuf = FALSE;
  stream->captured = FALSE;

  g_clear_handle_id (&stream->timeout_id, g_source_remove);
  connect_pw_stream (stream);
  stream->timeout_id = g_timeout_add (WINDOW_STREAM_TIMEOUT_MS,
                                      on_stream_timeout_cb,
                                      stream);
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

static void release_session (ScreenCastThumbnailer *self);

static gboolean
on_timeout_cb (gpointer user_data)
{
  ScreenCastThumbnailer *self = user_data;

  self->timeout_id = 0;
  g_debug ("%u window thumbnails still pending, giving up", self->n_pending);
  screen_cast_thumbnailer_stop (self);

  return G_SOURCE_REMOVE;
}

static void
fail_session_setup (ScreenCastThumbnailer *self,
                    GError                *error)
{
  if (self->done)
    return;

  g_warning ("Failed to capture window thumbnails: %s", error->message);
  screen_cast_thumbnailer_stop (self);
}

static void
on_session_started (GObject      *source_object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  g_autoptr(ScreenCastThumbnailer) self = user_data;
  g_autoptr(GError) error = NULL;

  if (!org_gnome_mutter_screen_cast_session_call_start_finish (ORG_GNOME_MUTTER_SCREEN_CAST_SESSION (source_object),
                                                               result,
                                                               &error))
    {
      fail_session_setup (self, error);
      return;
    }

  if (self->done)
    return;

  self->start_time = g_get_monotonic_time ();
  g_debug ("Window thumbnail session for %u windows set up in %" G_GINT64_FORMAT " ms",
           self->n_pending, (self->start_time - self->setup_start_time) / 1000);
  self->timeout_id =
    g_timeout_add (SESSION_EXTRA_TIMEOUT_MS +
                   WINDOW_STREAM_TIMEOUT_MS * ((self->n_pending + MAX_ACTIVE_WINDOW_STREAMS - 1) /
                                               MAX_ACTIVE_WINDOW_STREAMS),
                   on_timeout_cb, self);
}

typedef struct
{
  ScreenCastThumbnailer *thumbnailer;
  uint64_t window_id;
  char *title;
} RecordWindowData;

static void
record_window_data_free (RecordWindowData *data)
{
  g_clear_object (&data->thumbnailer);
  g_free (data->title);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (RecordWindowData, record_window_data_free)

static void
add_stream (ScreenCastThumbnailer *self,
            uint64_t               window_id,
            const char            *stream_path)
{
  ThumbnailStream *stream;

  stream = g_new0 (ThumbnailStream, 1);
  stream->thumbnailer = self;
  stream->window_id = window_id;
  stream->node_id = SPA_ID_INVALID;

  /* Mutter's unique name and no properties need no round trips; the signal
   * subscription reaches the bus before Start does */
  stream->proxy =
    org_gnome_mutter_screen_cast_stream_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (self->session)),
                                                        G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START |
                                                        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                        g_dbus_proxy_get_name (G_DBUS_PROXY (self->session)),
                                                        stream_path,
                                                        NULL,
                                                        NULL);
  if (!stream->proxy)
    {
      thumbnail_stream_free (stream);
      return;
    }

  g_signal_connect (stream->proxy, "pipewire-stream-added",
                    G_CALLBACK (on_pipewire_stream_added),
                    stream);

  g_ptr_array_add (self->streams, stream);
}

static void
on_window_recorded (GObject      *source_object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  g_autoptr(RecordWindowData) data = user_data;
  ScreenCastThumbnailer *self = data->thumbnailer;
  g_autoptr(GError) error = NULL;
  g_autofree char *stream_path = NULL;

  if (!org_gnome_mutter_screen_cast_session_call_record_window_finish (ORG_GNOME_MUTTER_SCREEN_CAST_SESSION (source_object),
                                                                       &stream_path,
                                                                       result,
                                                                       &error))
    {
      if (!self->done)
        g_debug ("Not thumbnailing window %s: %s", data->title, error->message);
    }
  else if (!self->done)
    {
      add_stream (self, data->window_id, stream_path);
    }

  if (self->done || --self->n_recording > 0)
    return;

  self->n_pending = self->streams->len;
  if (self->n_pending == 0)
    {
      screen_cast_thumbnailer_stop (self);
      return;
    }

  org_gnome_mutter_screen_cast_session_call_start (self->session,
                                                   NULL,
                                                   on_session_started,
                                                   g_object_ref (self));
}

static void
record_windows (ScreenCastThumbnailer *self)
{
  unsigned int i;

  /* All in flight at once, rather than a round trip each */
  self->n_recording = self->windows->len;
  for (i = 0; i < self->windows->len; i++)
    {
      ShellWindow *window = g_ptr_array_index (self->windows, i);
      GVariantBuilder properties_builder;
      RecordWindowData *data;

      data = g_new0 (RecordWindowData, 1);
      data->thumbnailer = g_object_ref (self);
      data->window_id = shell_window_get_id (window);
      data->title = g_strdup (shell_window_get_title (window));

      g_variant_builder_init (&properties_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&properties_builder, "{sv}",
                             "window-id", g_variant_new_uint64 (data->window_id));

      org_gnome_mutter_screen_cast_session_call_record_window (self->session,
                                                               g_variant_builder_end (&properties_builder),
                                                               NULL,
                                                               on_window_recorded,
                                                               data);
    }

  g_clear_pointer (&self->windows, g_ptr_array_unref);
}

static void
on_session_created (GObject      *source_object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  g_autoptr(ScreenCastThumbnailer) self = user_data;
  GDBusProxy *screen_cast = G_DBUS_PROXY (source_object);
  g_autoptr(GError) error = NULL;
  g_autofree char *session_path = NULL;

  if (!org_gnome_mutter_screen_cast_call_create_session_finish (ORG_GNOME_MUTTER_SCREEN_CAST (screen_cast),
                                                                &session_path,
                                                                result,
                                                                &error))
    {
      fail_session_setup (self, error);
      return;
    }

  self->session =
    org_gnome_mutter_screen_cast_session_proxy_new_sync (g_dbus_proxy_get_connection (screen_cast),
                                                         G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START |
                                                         G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                                                         G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                                         g_dbus_proxy_get_name_owner (screen_cast),
                                                         session_path,
                                                         NULL,
                                                         &error);
  if (!self->session)
    {
      fail_session_setup (self, error);
      return;
    }

  /* Stopped while the session was being created; don't leave it behind */
  if (self->done)
    {
      release_session (self);
      return;
    }

  if (!setup_pipewire (self, &error))
    {
      fail_session_setup (self, error);
      return;
    }

  record_windows (self);
}

static void
on_screen_cast_proxy_ready (GObject      *source_object,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  g_autoptr(ScreenCastThumbnailer) self = user_data;
  g_autoptr(OrgGnomeMutterScreenCast) screen_cast = NULL;
  g_autoptr(GError) error = NULL;
  GVariantBuilder properties_builder;

  screen_cast = org_gnome_mutter_screen_cast_proxy_new_for_bus_finish (result, &error);
  if (!screen_cast)
    {
      fail_session_setup (self, error);
      return;
    }

  if (self->done)
    return;

  if (!g_dbus_proxy_get_name_owner (G_DBUS_PROXY (screen_cast)))
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "org.gnome.Mutter.ScreenCast is not running");
      fail_session_setup (self, error);
      return;
    }

  g_variant_builder_init (&properties_builder, G_VARIANT_TYPE_VARDICT);
  org_gnome_mutter_screen_cast_call_create_session (screen_cast,
                                                    g_variant_builder_end (&properties_builder),
                                                    NULL,
                                                    on_session_created,
                                                    g_steal_pointer (&self));
}

static void
release_session (ScreenCastThumbnailer *self)
{
  g_clear_handle_id (&self->timeout_id, g_source_remove);

  teardown_pipewire (self);
  g_queue_clear (&self->streams_to_connect);
  g_ptr_array_set_size (self->streams, 0);
  g_clear_pointer (&self->windows, g_ptr_array_unref);
  g_clear_object (&self->renderer);

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
 * @renderer: (nullable): renders the thumbnails down to size
 *
 * Records each window once and emits `thumbnail-ready` per captured frame,
 * then `done` when all frames arrived, on timeout, or on failure. Can only be
 * started once.
 */
void
screen_cast_thumbnailer_start (ScreenCastThumbnailer *self,
                               GListModel            *windows,
                               GskRenderer           *renderer)
{
  unsigned int i;

  g_return_if_fail (SCREEN_CAST_IS_THUMBNAILER (self));

  if (self->started)
    return;

  self->started = TRUE;
  g_set_object (&self->renderer, renderer);

  self->windows = g_ptr_array_new_with_free_func (g_object_unref);
  for (i = 0; i < g_list_model_get_n_items (windows); i++)
    g_ptr_array_add (self->windows, g_list_model_get_item (windows, i));

  if (self->windows->len == 0)
    {
      screen_cast_thumbnailer_stop (self);
      return;
    }

  self->setup_start_time = g_get_monotonic_time ();
  org_gnome_mutter_screen_cast_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                                  G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START |
                                                  G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                                                  G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                                  "org.gnome.Mutter.ScreenCast",
                                                  "/org/gnome/Mutter/ScreenCast",
                                                  NULL,
                                                  on_screen_cast_proxy_ready,
                                                  g_object_ref (self));
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
