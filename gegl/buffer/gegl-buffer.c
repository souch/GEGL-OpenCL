/* This file is part of GEGL.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright 2006-2008 Øyvind Kolås <pippin@gimp.org>
 */

#include "config.h"

#include <math.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef G_OS_WIN32
#include <process.h>
#define getpid() _getpid()
#endif

#include <glib-object.h>
#include <glib/gstdio.h>
#include <glib/gprintf.h>
#include <gio/gio.h>

#include "gegl.h"
#include "gegl-types-internal.h"
#include "gegl-buffer-types.h"
#include "gegl-buffer.h"
#include "gegl-buffer-private.h"
#include "gegl-tile-handler.h"
#include "gegl-tile-storage.h"
#include "gegl-tile-backend.h"
#include "gegl-tile-backend-file.h"
#include "gegl-tile-backend-tiledir.h"
#include "gegl-tile-backend-ram.h"
#include "gegl-tile.h"
#include "gegl-tile-handler-cache.h"
#include "gegl-tile-handler-log.h"
#include "gegl-tile-handler-empty.h"
#include "gegl-sampler-nearest.h"
#include "gegl-sampler-linear.h"
#include "gegl-sampler-cubic.h"
#include "gegl-sampler-nohalo.h"
#include "gegl-sampler-lohalo.h"
#include "gegl-types-internal.h"
#include "gegl-utils.h"
#include "gegl-id-pool.h"
#include "gegl-buffer-index.h"
#include "gegl-config.h"
#include "gegl-buffer-cl-cache.h"

/* #define GEGL_BUFFER_DEBUG_ALLOCATIONS */

/* #define GEGL_BUFFER_DEBUG_ALLOCATIONS to print allocation stack
 * traces for leaked GeglBuffers using GNU C libs backtrace_symbols()
 */
#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif


G_DEFINE_TYPE (GeglBuffer, gegl_buffer, GEGL_TYPE_TILE_HANDLER)

static GObjectClass * parent_class = NULL;

enum
{
  PROP_0,
  PROP_X,
  PROP_Y,
  PROP_WIDTH,
  PROP_HEIGHT,
  PROP_SHIFT_X,
  PROP_SHIFT_Y,
  PROP_ABYSS_X,
  PROP_ABYSS_Y,
  PROP_ABYSS_WIDTH,
  PROP_ABYSS_HEIGHT,
  PROP_TILE_WIDTH,
  PROP_TILE_HEIGHT,
  PROP_FORMAT,
  PROP_PX_SIZE,
  PROP_PIXELS,
  PROP_PATH,
  PROP_BACKEND
};

enum {
  CHANGED,
  LAST_SIGNAL
};

static char * get_next_swap_path (void);

static const void *gegl_buffer_internal_get_format (GeglBuffer *buffer);


guint gegl_buffer_signals[LAST_SIGNAL] = { 0 };


static inline gint gegl_buffer_needed_tiles (gint w,
                                             gint stride)
{
  return ((w - 1) / stride) + 1;
}

static inline gint gegl_buffer_needed_width (gint w,
                                             gint stride)
{
  return gegl_buffer_needed_tiles (w, stride) * stride;
}

static void
gegl_buffer_get_property (GObject    *gobject,
                          guint       property_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  GeglBuffer *buffer = GEGL_BUFFER (gobject);

  switch (property_id)
    {
      case PROP_WIDTH:
        g_value_set_int (value, buffer->extent.width);
        break;

      case PROP_HEIGHT:
        g_value_set_int (value, buffer->extent.height);
        break;
      case PROP_TILE_WIDTH:
        g_value_set_int (value, buffer->tile_width);
        break;

      case PROP_TILE_HEIGHT:
        g_value_set_int (value, buffer->tile_height);
        break;

      case PROP_PATH:
          {
            GeglTileBackend *backend = gegl_buffer_backend (buffer);
            if (GEGL_IS_TILE_BACKEND_FILE(backend))
              {
                if (buffer->path)
                  g_free (buffer->path);
                buffer->path = NULL;
                g_object_get (backend, "path", &buffer->path, NULL);
              }
          }
          g_value_set_string (value, buffer->path);
        break;
      case PROP_PIXELS:
        g_value_set_int (value, buffer->extent.width * buffer->extent.height);
        break;

      case PROP_PX_SIZE:
        g_value_set_int (value, buffer->tile_storage->px_size);
        break;

      case PROP_FORMAT:
        /* might already be set the first time, if it was set during
         * construction, we're caching the value in the buffer itself,
         * since it will never change.
         */

        {
          const Babl *format;

            format = buffer->soft_format;
          if (format == NULL)
            format = buffer->format;
          if (format == NULL)
            format = gegl_buffer_internal_get_format (buffer);

          g_value_set_pointer (value, (void*)format);
        }
        break;

      case PROP_BACKEND:
        g_value_set_pointer (value, buffer->backend);
        break;

      case PROP_X:
        g_value_set_int (value, buffer->extent.x);
        break;

      case PROP_Y:
        g_value_set_int (value, buffer->extent.y);
        break;

      case PROP_SHIFT_X:
        g_value_set_int (value, buffer->shift_x);
        break;

      case PROP_SHIFT_Y:
        g_value_set_int (value, buffer->shift_y);
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, property_id, pspec);
        break;
    }
}

static void
gegl_buffer_set_property (GObject      *gobject,
                          guint         property_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  GeglBuffer *buffer = GEGL_BUFFER (gobject);

  switch (property_id)
    {
      case PROP_X:
        buffer->extent.x = g_value_get_int (value);
        break;

      case PROP_Y:
        buffer->extent.y = g_value_get_int (value);
        break;

      case PROP_WIDTH:
        buffer->extent.width = g_value_get_int (value);
        break;

      case PROP_HEIGHT:
        buffer->extent.height = g_value_get_int (value);
        break;

      case PROP_TILE_HEIGHT:
        buffer->tile_height = g_value_get_int (value);
        break;

      case PROP_TILE_WIDTH:
        buffer->tile_width = g_value_get_int (value);
        break;

      case PROP_PATH:
        if (buffer->path)
          g_free (buffer->path);
        buffer->path = g_value_dup_string (value);
        break;

      case PROP_SHIFT_X:
        buffer->shift_x = g_value_get_int (value);
        break;

      case PROP_SHIFT_Y:
        buffer->shift_y = g_value_get_int (value);
        break;

      case PROP_ABYSS_X:
        buffer->abyss.x = g_value_get_int (value);
        break;

      case PROP_ABYSS_Y:
        buffer->abyss.y = g_value_get_int (value);
        break;

      case PROP_ABYSS_WIDTH:
        buffer->abyss.width = g_value_get_int (value);
        break;

      case PROP_ABYSS_HEIGHT:
        buffer->abyss.height = g_value_get_int (value);
        break;

      case PROP_FORMAT:
        /* Do not set to NULL even if asked to do so by a non-overriden
         * value, this is needed since a default value can not be specified
         * for a gpointer paramspec
         */
        if (g_value_get_pointer (value))
          {
            const Babl *format = g_value_get_pointer (value);
            /* XXX: need to check if the internal format matches, should
             * perhaps do different things here depending on whether
             * we are during construction or not
             */
            if (buffer->soft_format)
              {
                gegl_buffer_set_format (buffer, format);
              }
            else
              {
                buffer->format = format;
              }
          }
        break;
      case PROP_BACKEND:
        if (g_value_get_pointer (value))
          buffer->backend = g_value_get_pointer (value);
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, property_id, pspec);
        break;
    }
}

#ifdef GEGL_BUFFER_DEBUG_ALLOCATIONS
static GList *allocated_buffers_list = NULL;
#endif
static gint   allocated_buffers      = 0;
static gint   de_allocated_buffers   = 0;

/* this should only be possible if this buffer matches all the buffers down to
 * storage, all of those parent buffers would change size as well, no tiles
 * would be voided as a result of changing the extent.
 */
gboolean
gegl_buffer_set_extent (GeglBuffer          *buffer,
                        const GeglRectangle *extent)
{
  g_return_val_if_fail(GEGL_IS_BUFFER(buffer), FALSE);
   (*(GeglRectangle*)gegl_buffer_get_extent (buffer))=*extent;

  if ((GeglBufferHeader*)(gegl_buffer_backend (buffer)->priv->header))
    {
      GeglBufferHeader *header =
        ((GeglBufferHeader*)(gegl_buffer_backend (buffer)->priv->header));
      header->x = buffer->extent.x;
      header->y = buffer->extent.y;
      header->width = buffer->extent.width;
      header->height = buffer->extent.height;
    }

  if (buffer->abyss_tracks_extent)
    {
      buffer->abyss = *extent;
    }

  return TRUE;
}

gboolean
gegl_buffer_set_abyss (GeglBuffer          *buffer,
                       const GeglRectangle *abyss)
{
  g_return_val_if_fail(GEGL_IS_BUFFER(buffer), FALSE);

  buffer->abyss = *abyss;

  return TRUE;
}

void gegl_buffer_stats (void)
{
  g_warning ("Buffer statistics: allocated:%i deallocated:%i balance:%i",
             allocated_buffers, de_allocated_buffers, allocated_buffers - de_allocated_buffers);
}

gint gegl_buffer_leaks (void)
{
#ifdef GEGL_BUFFER_DEBUG_ALLOCATIONS
  {
    GList *leaked_buffer = NULL;

    for (leaked_buffer = allocated_buffers_list;
         leaked_buffer != NULL;
         leaked_buffer = leaked_buffer->next)
      {
        GeglBuffer *buffer = GEGL_BUFFER (leaked_buffer->data);

        g_printerr ("\n"
                   "Leaked buffer allocation stack trace:\n");
        g_printerr ("%s\n", buffer->alloc_stack_trace);
      }
  }
  g_list_free (allocated_buffers_list);
  allocated_buffers_list = NULL;
#endif

  return allocated_buffers - de_allocated_buffers;
}

void
_gegl_buffer_drop_hot_tile (GeglBuffer *buffer)
{
  GeglTileStorage *storage = buffer->tile_storage;
  if (storage->hot_tile)
    {
      gegl_tile_unref (storage->hot_tile);
      storage->hot_tile = NULL;
    }
}

static void
gegl_buffer_dispose (GObject *object)
{
  GeglBuffer  *buffer  = GEGL_BUFFER (object);
  GeglTileHandler *handler = GEGL_TILE_HANDLER (object);

  gegl_buffer_sample_cleanup (buffer);

  if (gegl_cl_is_accelerated ())
    gegl_buffer_cl_cache_invalidate (GEGL_BUFFER (object), NULL);

  if (handler->source &&
      GEGL_IS_TILE_STORAGE (handler->source))
    {
      GeglTileBackend *backend = gegl_buffer_backend (buffer);

      /* only flush non-internal backends,. */
      if (!(GEGL_IS_TILE_BACKEND_FILE (backend) ||
            GEGL_IS_TILE_BACKEND_RAM (backend) ||
            GEGL_IS_TILE_BACKEND_TILE_DIR (backend)))
        gegl_buffer_flush (buffer);

      gegl_tile_source_reinit (GEGL_TILE_SOURCE (handler->source));

#if 0
      g_object_unref (handler->source);
      handler->source = NULL; /* this might be a dangerous way of marking that we have already voided */
#endif
    }

  _gegl_buffer_drop_hot_tile (buffer);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gegl_buffer_finalize (GObject *object)
{
#ifdef GEGL_BUFFER_DEBUG_ALLOCATIONS
  g_free (GEGL_BUFFER (object)->alloc_stack_trace);
  allocated_buffers_list = g_list_remove (allocated_buffers_list, object);
#endif

  g_free (GEGL_BUFFER (object)->path);
  de_allocated_buffers++;
  G_OBJECT_CLASS (parent_class)->finalize (object);
}


GeglTileBackend *
gegl_buffer_backend2 (GeglBuffer *buffer)
{
  GeglTileSource *tmp = GEGL_TILE_SOURCE (buffer);

  if (!tmp)
    return NULL;

  do
    {
      tmp = GEGL_TILE_HANDLER (tmp)->source;
    } while (tmp &&
             !GEGL_IS_TILE_BACKEND (tmp));
  if (!tmp &&
      !GEGL_IS_TILE_BACKEND (tmp))
    return NULL;

  return (GeglTileBackend *) tmp;
}

GeglTileBackend *
gegl_buffer_backend (GeglBuffer *buffer)
{
  GeglTileBackend *tmp;
  if (G_LIKELY (buffer->backend))
    return buffer->backend;

  tmp = gegl_buffer_backend2 (buffer);

  if (tmp)
    buffer->backend = tmp;

  return (GeglTileBackend *) tmp;
}

static GeglTileStorage *
gegl_buffer_tile_storage (GeglBuffer *buffer)
{
  GeglTileSource *tmp = (GeglTileSource*) (buffer);

  do tmp = ((GeglTileHandler *) (tmp))->source;
  while (!GEGL_IS_TILE_STORAGE (tmp));

  g_assert (tmp);

  return (GeglTileStorage *) tmp;
}

static void gegl_buffer_storage_changed (GeglTileStorage     *storage,
                                         const GeglRectangle *rect,
                                         gpointer             userdata)
{
  g_signal_emit_by_name (GEGL_BUFFER (userdata), "changed", rect, NULL);
}

static GObject *
gegl_buffer_constructor (GType                  type,
                         guint                  n_params,
                         GObjectConstructParam *params)
{
  GObject         *object;
  GeglBuffer      *buffer;
  GeglTileBackend *backend;
  GeglTileHandler *handler;
  GeglTileSource  *source;

  object = G_OBJECT_CLASS (parent_class)->constructor (type, n_params, params);

  buffer  = GEGL_BUFFER (object);
  handler = GEGL_TILE_HANDLER (object);
  source  = handler->source;
  backend = gegl_buffer_backend (buffer);

  if (source)
    {
      if (GEGL_IS_TILE_STORAGE (source))
        buffer->format = GEGL_TILE_STORAGE (source)->format;
      else if (GEGL_IS_BUFFER (source))
        buffer->format = GEGL_BUFFER (source)->format;
    }
  else
    {
      if (buffer->backend)
        {
          backend = buffer->backend;
          buffer->format = gegl_tile_backend_get_format (backend);
        }
      else
        {
          gboolean use_ram = FALSE;
          const char *maybe_path = NULL;

          if (!buffer->format)
            {
              g_warning ("Buffer constructed without format, assuming RGBA float");
              buffer->format = babl_format("RGBA float");
            }

          /* make a new backend & storage */

          if (buffer->path)
            maybe_path = buffer->path;
          else
            maybe_path = gegl_config ()->swap;

          if (maybe_path)
            use_ram = g_ascii_strcasecmp (maybe_path, "ram") == 0;
          else
            use_ram = TRUE;

          if (use_ram == TRUE)
            {
              backend = g_object_new (GEGL_TYPE_TILE_BACKEND_RAM,
                                      "tile-width", buffer->tile_width,
                                      "tile-height", buffer->tile_height,
                                      "format", buffer->format,
                                      NULL);
            }
          else
            {
              if (buffer->path)
                {
                  backend = g_object_new (GEGL_TYPE_TILE_BACKEND_FILE,
                                          "tile-width", buffer->tile_width,
                                          "tile-height", buffer->tile_height,
                                          "format", buffer->format,
                                          "path", buffer->path,
                                          NULL);
                }
              else
                {
                  gchar *path = get_next_swap_path();

                  backend = g_object_new (GEGL_TYPE_TILE_BACKEND_FILE,
                                          "tile-width", buffer->tile_width,
                                          "tile-height", buffer->tile_height,
                                          "format", buffer->format,
                                          "path", path,
                                          NULL);
                  g_free (path);
                }
            }

          buffer->backend = backend;
        }

      source = GEGL_TILE_SOURCE (gegl_tile_storage_new (backend));
      gegl_tile_handler_set_source ((GeglTileHandler*)(buffer), source);
      g_object_unref (source);
      g_object_unref (backend);
    }

   /* Connect to the changed signal of source, this is used by some backends
    * (e.g. File) to notify of outside changes to the buffer.
    */
  if (GEGL_IS_TILE_STORAGE (source))
    {
      g_signal_connect (source, "changed",
                        G_CALLBACK (gegl_buffer_storage_changed),
                        buffer);
    }

  g_assert (backend);

  if (buffer->extent.width == -1 ||
      buffer->extent.height == -1) /* no specified extents,
                                      inheriting from source */
    {
      if (GEGL_IS_BUFFER (source))
        {
          buffer->extent.x = GEGL_BUFFER (source)->extent.x;
          buffer->extent.y = GEGL_BUFFER (source)->extent.y;
          buffer->extent.width  = GEGL_BUFFER (source)->extent.width;
          buffer->extent.height = GEGL_BUFFER (source)->extent.height;
        }
      else if (GEGL_IS_TILE_STORAGE (source))
        {
          buffer->extent.x = 0;
          buffer->extent.y = 0;
          buffer->extent.width  = GEGL_TILE_STORAGE (source)->width;
          buffer->extent.height = GEGL_TILE_STORAGE (source)->height;
        }
      else
        {
          buffer->extent.x = 0;
          buffer->extent.y = 0;
          buffer->extent.width  = 0;
          buffer->extent.height = 0;
        }
    }

  buffer->abyss_tracks_extent = FALSE;

  if (buffer->abyss.width == 0 &&
      buffer->abyss.height == 0 &&
      buffer->abyss.x == 0 &&
      buffer->abyss.y == 0)      /* 0 sized extent == inherit buffer extent
                                  */
    {
      buffer->abyss.x             = buffer->extent.x;
      buffer->abyss.y             = buffer->extent.y;
      buffer->abyss.width         = buffer->extent.width;
      buffer->abyss.height        = buffer->extent.height;
      buffer->abyss_tracks_extent = TRUE;
    }
  else if (buffer->abyss.width == 0 &&
           buffer->abyss.height == 0)
    {
      g_warning ("peculiar abyss dimensions: %i,%i %ix%i",
                 buffer->abyss.x,
                 buffer->abyss.y,
                 buffer->abyss.width,
                 buffer->abyss.height);
    }
  else if (buffer->abyss.width == -1 ||
           buffer->abyss.height == -1)
    {
      buffer->abyss.x      = GEGL_BUFFER (source)->abyss.x - buffer->shift_x;
      buffer->abyss.y      = GEGL_BUFFER (source)->abyss.y - buffer->shift_y;
      buffer->abyss.width  = GEGL_BUFFER (source)->abyss.width;
      buffer->abyss.height = GEGL_BUFFER (source)->abyss.height;
    }

  /* intersect our own abyss with parent's abyss if it exists
   */
  if (GEGL_IS_BUFFER (source))
    {
      GeglRectangle parent;
      GeglRectangle request;
      GeglRectangle self;

      parent.x = GEGL_BUFFER (source)->abyss.x - buffer->shift_x;
      parent.y = GEGL_BUFFER (source)->abyss.y - buffer->shift_y;
      parent.width = GEGL_BUFFER (source)->abyss.width;
      parent.height = GEGL_BUFFER (source)->abyss.height;

      request.x = buffer->abyss.x;
      request.y = buffer->abyss.y;
      request.width = buffer->abyss.width;
      request.height = buffer->abyss.height;

      gegl_rectangle_intersect (&self, &parent, &request);

      /* Don't have the abyss track the extent if the intersection is
       * not the entire extent. Otherwise, setting the extent identical
       * to itself could suddenly make the abyss bigger. */
      if (buffer->abyss_tracks_extent &&
          (buffer->extent.x      != self.x ||
           buffer->extent.y      != self.y ||
           buffer->extent.width  != self.width ||
           buffer->extent.height != self.height) )
        {
          buffer->abyss_tracks_extent = FALSE;
        }

      buffer->abyss.x      = self.x;
      buffer->abyss.y      = self.y;
      buffer->abyss.width  = self.width;
      buffer->abyss.height = self.height;
    }

  /* compute our own total shift <- this should probably happen
   * approximatly first */
  if (GEGL_IS_BUFFER (source))
    {
      GeglBuffer *source_buf;

      source_buf = GEGL_BUFFER (source);

      buffer->shift_x += source_buf->shift_x;
      buffer->shift_y += source_buf->shift_y;
    }
  else
    {
    }

  buffer->tile_storage = gegl_buffer_tile_storage (buffer);

  /* intialize the soft format to be equivalent to the actual
   * format
   */
  buffer->soft_format = buffer->format;

  return object;
}

static GeglTile *
gegl_buffer_get_tile (GeglTileSource *source,
                      gint        x,
                      gint        y,
                      gint        z)
{
  GeglTileHandler *handler = (GeglTileHandler*) (source);
  GeglTile    *tile   = NULL;
  source = handler->source;

  if (source)
    tile = gegl_tile_source_get_tile (source, x, y, z);
  else
    g_assert (0);

  if (tile)
    {
      GeglBuffer *buffer = (GeglBuffer*) (handler);

      /* storing information in tile, to enable the dispose function of the
       * tile instance to "hook" back to the storage with correct
       * coordinates.
       */
      {
        if (!tile->tile_storage)
          {
            gegl_tile_lock (tile);
            tile->tile_storage = buffer->tile_storage;
            gegl_tile_unlock (tile);
            tile->rev--;
          }
        tile->x = x;
        tile->y = y;
        tile->z = z;
      }
    }

  return tile;
}


static gpointer
gegl_buffer_command (GeglTileSource *source,
                     GeglTileCommand command,
                     gint            x,
                     gint            y,
                     gint            z,
                     gpointer        data)
{
  GeglTileHandler *handler = GEGL_TILE_HANDLER (source);
  switch (command)
    {
      case GEGL_TILE_GET:
        return gegl_buffer_get_tile (source, x, y, z);
      default:
        return gegl_tile_handler_source_command (handler, command, x, y, z, data);
    }
}

static void
gegl_buffer_class_init (GeglBufferClass *class)
{
  GObjectClass      *gobject_class       = G_OBJECT_CLASS (class);
  parent_class                = g_type_class_peek_parent (class);
  gobject_class->dispose      = gegl_buffer_dispose;
  gobject_class->finalize     = gegl_buffer_finalize;
  gobject_class->constructor  = gegl_buffer_constructor;
  gobject_class->set_property = gegl_buffer_set_property;
  gobject_class->get_property = gegl_buffer_get_property;

  g_object_class_install_property (gobject_class, PROP_PX_SIZE,
                                   g_param_spec_int ("px-size", "pixel-size", "size of a single pixel in bytes.",
                                                     0, G_MAXINT, 0,
                                                     G_PARAM_READABLE));
  g_object_class_install_property (gobject_class, PROP_PIXELS,
                                   g_param_spec_int ("pixels", "pixels", "total amount of pixels in image (width x height)",
                                                     0, G_MAXINT, 0,
                                                     G_PARAM_READABLE));
  g_object_class_install_property (gobject_class, PROP_WIDTH,
                                   g_param_spec_int ("width", "width", "pixel width of buffer",
                                                     -1, G_MAXINT, -1,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT));
  g_object_class_install_property (gobject_class, PROP_HEIGHT,
                                   g_param_spec_int ("height", "height", "pixel height of buffer",
                                                     -1, G_MAXINT, -1,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT));
  g_object_class_install_property (gobject_class, PROP_X,
                                   g_param_spec_int ("x", "x", "local origin's offset relative to source origin",
                                                     G_MININT / 2, G_MAXINT / 2, 0,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT));
  g_object_class_install_property (gobject_class, PROP_Y,
                                   g_param_spec_int ("y", "y", "local origin's offset relative to source origin",
                                                     G_MININT / 2, G_MAXINT / 2, 0,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT));
  g_object_class_install_property (gobject_class, PROP_ABYSS_WIDTH,
                                   g_param_spec_int ("abyss-width", "abyss-width", "pixel width of abyss",
                                                     -1, G_MAXINT, 0,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (gobject_class, PROP_ABYSS_HEIGHT,
                                   g_param_spec_int ("abyss-height", "abyss-height", "pixel height of abyss",
                                                     -1, G_MAXINT, 0,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (gobject_class, PROP_ABYSS_X,
                                   g_param_spec_int ("abyss-x", "abyss-x", "",
                                                     G_MININT / 2, G_MAXINT / 2, 0,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (gobject_class, PROP_ABYSS_Y,
                                   g_param_spec_int ("abyss-y", "abyss-y", "",
                                                     G_MININT / 2, G_MAXINT / 2, 0,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (gobject_class, PROP_SHIFT_X,
                                   g_param_spec_int ("shift-x", "shift-x", "",
                                                     G_MININT / 2, G_MAXINT / 2, 0,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (gobject_class, PROP_SHIFT_Y,
                                   g_param_spec_int ("shift-y", "shift-y", "",
                                                     G_MININT / 2, G_MAXINT / 2, 0,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (gobject_class, PROP_FORMAT,
                                   g_param_spec_pointer ("format", "format", "babl format",
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class, PROP_BACKEND,
                                   g_param_spec_pointer ("backend", "backend", "A custom tile-backend instance to use",
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class, PROP_TILE_HEIGHT,
                                   g_param_spec_int ("tile-height", "tile-height", "height of a tile",
                                                     -1, G_MAXINT, gegl_config()->tile_height,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (gobject_class, PROP_TILE_WIDTH,
                                   g_param_spec_int ("tile-width", "tile-width", "width of a tile",
                                                     -1, G_MAXINT, gegl_config()->tile_width,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (gobject_class, PROP_PATH,
                                   g_param_spec_string ("path", "Path",
                                                        "URI to where the buffer is stored",
                                     NULL, G_PARAM_READWRITE|G_PARAM_CONSTRUCT));

  gegl_buffer_signals[CHANGED] =
        g_signal_new ("changed",
                  G_TYPE_FROM_CLASS (gobject_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__BOXED,
                  G_TYPE_NONE, 1,
                  GEGL_TYPE_RECTANGLE);

}

#ifdef GEGL_BUFFER_DEBUG_ALLOCATIONS
#define MAX_N_FUNCTIONS 100
static gchar *
gegl_buffer_get_alloc_stack (void)
{
  char  *result         = NULL;
#ifndef HAVE_EXECINFO_H
  result = g_strdup ("backtrace() not available for this platform\n");
#else
  void  *functions[MAX_N_FUNCTIONS];
  int    n_functions    = 0;
  char **function_names = NULL;
  int    i              = 0;
  int    result_size    = 0;

  n_functions = backtrace (functions, MAX_N_FUNCTIONS);
  function_names = backtrace_symbols (functions, n_functions);

  for (i = 0; i < n_functions; i++)
    {
      result_size += strlen (function_names[i]);
      result_size += 1; /* for '\n' */
    }

  result = g_new (gchar, result_size + 1);
  result[0] = '\0';

  for (i = 0; i < n_functions; i++)
    {
      strcat (result, function_names[i]);
      strcat (result, "\n");
    }

  free (function_names);
#endif

  return result;
}
#endif

void gegl_bt (void);
void gegl_bt (void)
{
#ifdef GEGL_BUFFER_DEBUG_ALLOCATIONS
  g_print ("%s\n", gegl_buffer_get_alloc_stack ());
#endif
}

static void
gegl_buffer_init (GeglBuffer *buffer)
{
  buffer->tile_width = 128;
  buffer->tile_height = 64;
  ((GeglTileSource*)buffer)->command = gegl_buffer_command;

  allocated_buffers++;

#ifdef GEGL_BUFFER_DEBUG_ALLOCATIONS
  buffer->alloc_stack_trace = gegl_buffer_get_alloc_stack ();
  allocated_buffers_list = g_list_prepend (allocated_buffers_list, buffer);
#endif
}



const GeglRectangle *
gegl_buffer_get_extent (GeglBuffer *buffer)
{
  g_return_val_if_fail (GEGL_IS_BUFFER (buffer), NULL);

  return &(buffer->extent);
}


GeglBuffer *
gegl_buffer_new_ram (const GeglRectangle *extent,
                     const Babl          *format)
{
  GeglRectangle empty={0,0,0,0};

  if (extent==NULL)
    extent = &empty;

  if (format==NULL)
    format = babl_format ("RGBA float");

  return g_object_new (GEGL_TYPE_BUFFER,
                       "x", extent->x,
                       "y", extent->y,
                       "width", extent->width,
                       "height", extent->height,
                       "format", format,
                       "path", "RAM",
                       NULL);
}

GeglBuffer *
gegl_buffer_introspectable_new (const char *format_name,
                                gint x,
                                gint y,
                                gint width,
                                gint height)
{
  const Babl *format = NULL;

  if (format_name)
    format = babl_format (format_name);
  if (!format)
    format = babl_format ("RGBA float");

  return g_object_new (GEGL_TYPE_BUFFER,
                       "x", x,
                       "y", y,
                       "width", width,
                       "height", height,
                       "format", format,
                       NULL);
}

GeglBuffer *
gegl_buffer_new (const GeglRectangle *extent,
                 const Babl          *format)
{
  GeglRectangle empty={0,0,0,0};

  if (extent==NULL)
    extent = &empty;

  if (format==NULL)
    format = babl_format ("RGBA float");

  return g_object_new (GEGL_TYPE_BUFFER,
                       "x", extent->x,
                       "y", extent->y,
                       "width", extent->width,
                       "height", extent->height,
                       "format", format,
                       NULL);
}

GeglBuffer *
gegl_buffer_new_for_backend (const GeglRectangle *extent,
                             void                *backend)
{
  GeglRectangle rect={0,0,0,0};
  const Babl *format;

  /* if no extent is passed in inherit from backend */
  if (extent==NULL)
    {
      extent = &rect;
      rect = gegl_tile_backend_get_extent (backend);
      /* if backend didnt have a rect, make it an infinite plane  */
      if (gegl_rectangle_is_empty (extent))
        rect = gegl_rectangle_infinite_plane ();
    }

  /* use the format of the backend */
  format = gegl_tile_backend_get_format (backend);

  return g_object_new (GEGL_TYPE_BUFFER,
                       "x", extent->x,
                       "y", extent->y,
                       "width", extent->width,
                       "height", extent->height,
                       "format", format,
                       "backend", backend,
                       NULL);
}

void
gegl_buffer_add_handler (GeglBuffer *buffer,
                         gpointer    handler)
{
  GeglTileHandlerChain *chain;

  g_return_if_fail (GEGL_IS_BUFFER (buffer));
  g_return_if_fail (GEGL_IS_TILE_HANDLER (handler));

  g_object_set (handler,
                "format", buffer->tile_storage->format,
                "tile-width", buffer->tile_storage->tile_width,
                "tile-height", buffer->tile_storage->tile_height,
                NULL);

  chain = GEGL_TILE_HANDLER_CHAIN (buffer->tile_storage);

  gegl_tile_handler_chain_add (chain, handler);

  /* XXX */
  chain->chain = g_slist_remove (chain->chain, handler);
  chain->chain = g_slist_insert (chain->chain, handler, 2);

  gegl_tile_handler_chain_bind (chain);
}

void
gegl_buffer_remove_handler (GeglBuffer *buffer,
                            gpointer    handler)
{
  GeglTileHandlerChain *chain;

  g_return_if_fail (GEGL_IS_BUFFER (buffer));
  g_return_if_fail (GEGL_IS_TILE_HANDLER (handler));

  chain = GEGL_TILE_HANDLER_CHAIN (buffer->tile_storage);

  g_return_if_fail (g_slist_find (chain->chain, handler));

  chain->chain = g_slist_remove (chain->chain, handler);
  g_object_unref (handler);

  gegl_tile_handler_chain_bind (chain);
}

/* FIXME: this function needs optimizing, perhaps keep a pool
 * of GeglBuffer shells that can be adapted to the needs
 * on runtime, and recycling them through a hashtable?
 */
GeglBuffer*
gegl_buffer_create_sub_buffer (GeglBuffer          *buffer,
                               const GeglRectangle *extent)
{
  g_return_val_if_fail (GEGL_IS_BUFFER (buffer), NULL);

#if 1
  if (extent == NULL || gegl_rectangle_equal (extent, &buffer->extent))
    {
      g_object_ref (buffer);
      return buffer;
    }
#else
   if (extent == NULL)
      extent = gegl_buffer_get_extent (buffer);
#endif

  if (extent->width < 0 || extent->height < 0)
    {
      g_warning ("avoiding creating buffer of size: %ix%i returning an empty buffer instead.\n", extent->width, extent->height);
      return g_object_new (GEGL_TYPE_BUFFER,
                           "source", buffer,
                           "x", extent->x,
                           "y", extent->y,
                           "width", 0,
                           "height", 0,
                           NULL);
    }
  return g_object_new (GEGL_TYPE_BUFFER,
                       "source", buffer,
                       "x", extent->x,
                       "y", extent->y,
                       "width", extent->width,
                       "height", extent->height,
                       NULL);
}

static char *
get_next_swap_path (void)
{
  static gint no = 1;

  gchar *filename;
  gchar *path;
  gchar *swap_dir = gegl_config()->swap;

  if (!swap_dir)
    g_error("Attempted to build a file buffer with no path and no swap directory");

  filename = g_strdup_printf ("%i-%i", getpid(), no);
  g_atomic_int_inc (&no);
  path = g_build_filename (swap_dir, filename, NULL);
  g_free (filename);

  return path;
}

static const void *gegl_buffer_internal_get_format (GeglBuffer *buffer)
{
  g_assert (buffer);
  if (buffer->format != NULL)
    return buffer->format;
  return gegl_tile_backend_get_format (gegl_buffer_backend (buffer));
}

const Babl *
gegl_buffer_get_format (GeglBuffer *buffer)
{
  return buffer?buffer->format:NULL;
}

const Babl *
gegl_buffer_set_format (GeglBuffer *buffer,
                        const Babl *format)
{
  if (format == NULL)
    {
      buffer->soft_format = buffer->format;
      return buffer->soft_format;
    }
  if (babl_format_get_bytes_per_pixel (format) ==
      babl_format_get_bytes_per_pixel (buffer->format))
    {
      buffer->soft_format = format;
      return buffer->soft_format;
    }
  g_warning ("tried to set format of different bpp on buffer\n");
  return NULL;
}

gboolean gegl_buffer_is_shared (GeglBuffer *buffer)
{
  GeglTileBackend *backend = gegl_buffer_backend (buffer);
  return backend->priv->shared;
}

gboolean gegl_buffer_try_lock (GeglBuffer *buffer)
{
  gboolean ret;
  GeglTileBackend *backend = gegl_buffer_backend (buffer);
  g_mutex_lock (&buffer->tile_storage->mutex);
  if (buffer->lock_count>0)
    {
      buffer->lock_count++;
      g_mutex_unlock (&buffer->tile_storage->mutex);
      return TRUE;
    }
  if (gegl_buffer_is_shared(buffer))
    ret =gegl_tile_backend_file_try_lock (GEGL_TILE_BACKEND_FILE (backend));
  else
    ret = TRUE;
  if (ret)
    buffer->lock_count++;
  g_mutex_unlock (&buffer->tile_storage->mutex);
  return TRUE;
}

/* this locking is for synchronising shared buffers */
gboolean gegl_buffer_lock (GeglBuffer *buffer)
{
  while (gegl_buffer_try_lock (buffer)==FALSE)
    {
      g_print ("waiting to aquire buffer..");
        g_usleep (100000);
    }
  return TRUE;
}

gboolean gegl_buffer_unlock (GeglBuffer *buffer)
{
  gboolean ret = TRUE;
  GeglTileBackend *backend = gegl_buffer_backend (buffer);
  g_mutex_lock (&buffer->tile_storage->mutex);
  g_assert (buffer->lock_count >=0);
  buffer->lock_count--;
  g_assert (buffer->lock_count >=0);
  if (buffer->lock_count == 0 && gegl_buffer_is_shared (buffer))
    {
      ret = gegl_tile_backend_file_unlock (GEGL_TILE_BACKEND_FILE (backend));
    }
  g_mutex_unlock (&buffer->tile_storage->mutex);
  return ret;
}

void gegl_buffer_emit_changed_signal(GeglBuffer *buffer, const GeglRectangle *rect)
{
  GeglRectangle copy;

  if (rect == NULL) {
    copy = *gegl_buffer_get_extent (buffer);
  } else {
    copy = *rect;
  }

  g_signal_emit(buffer, gegl_buffer_signals[CHANGED], 0, &copy, NULL);
}
