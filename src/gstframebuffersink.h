/* GStreamer GstFramebufferSink class
 * Copyright (C) 2013 Harm Hanemaaijer <fgenfg@yahoo.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef _GST_FRAMEBUFFERSINK_H_
#define _GST_FRAMEBUFFERSINK_H_

#include <stdint.h>
#include <linux/fb.h>
#include <gst/video/gstvideosink.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

typedef enum { GST_FRAMEBUFFERSINK_OVERLAY_TYPE_I420 = 0,
    GST_FRAMEBUFFERSINK_OVERLAY_TYPE_BGRX32,
    GST_FRAMEBUFFERSINK_OVERLAY_TYPE_NONE }
    GstFramebufferSinkOverlayType;
#define GST_FRAMEBUFFERSINK_OVERLAY_TYPE_FIRST GST_FRAMEBUFFERSINK_OVERLAY_TYPE_I420
#define GST_FRAMEBUFFERSINK_OVERLAY_TYPE_LAST GST_FRAMEBUFFERSINK_OVERLAY_TYPE_BGRX32

/* Forward declaration. */

typedef struct _GstFramebufferSinkAllocator GstFramebufferSinkAllocator;

/* Main class. */

#define GST_TYPE_FRAMEBUFFERSINK   (gst_framebuffersink_get_type())
#define GST_FRAMEBUFFERSINK(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_FRAMEBUFFERSINK,GstFramebufferSink))
#define GST_FRAMEBUFFERSINK_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_FRAMEBUFFERSINK,GstFramebufferSinkClass))
#define GST_IS_FRAMEBUFFERSINK(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_FRAMEBUFFERSINK))
#define GST_IS_FRAMEBUFFERSINK_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_FRAMEBUFFERSINK))
#define GST_FRAMEBUFFERSINK_GET_CLASS(klass) \
  (G_TYPE_INSTANCE_GET_CLASS ((klass), GST_TYPE_FRAMEBUFFER_SINK, GstFramebufferSinkClass))

typedef struct _GstFramebufferSink GstFramebufferSink;
typedef struct _GstFramebufferSinkClass GstFramebufferSinkClass;

struct _GstFramebufferSink
{
  GstVideoSink videosink; /* Includes width and height. */
  GstFramebufferSinkAllocator *allocator;

  /* Configurable properties. */
  gboolean silent;
  gboolean native_resolution;
  gboolean use_hardware_overlay;
  gboolean clear;
  gint requested_video_width;
  gint requested_video_height;
  gint width_before_scaling;
  gint height_before_scaling;
  gint fps;
  gboolean use_buffer_pool;
  gboolean vsync;
  gint flip_buffers;
  gboolean use_graphics_mode;
  gboolean pan_does_vsync;
  gboolean preserve_par;

  /* Invariant device parameters. */
  int fd;
  int saved_kd_mode;
  gchar *device;
  uint8_t *framebuffer;
  struct fb_fix_screeninfo fixinfo;
  int bytespp;
  uint32_t rmask, gmask, bmask;
  int endianness;
  GstVideoFormat framebuffer_format;
  int max_framebuffers;
  GQuark framebuffer_address_quark;
  gboolean open_hardware_success;
  uint8_t hardware_overlay_types_supported[GST_FRAMEBUFFERSINK_OVERLAY_TYPE_LAST -
      GST_FRAMEBUFFERSINK_OVERLAY_TYPE_FIRST + 1];
  /* Variable device parameters. */
  struct fb_var_screeninfo varinfo;
  int nu_framebuffers_used;
  int current_framebuffer_index;
  int nu_overlays_used;
  int current_overlay_index;
  int scaled_width, scaled_height;
  /* Video memory buffer allocation management. */
  gsize allocation_size;
  guintptr allocation_start_offset;
  int buffer_allocation_table_size;
  uint8_t *buffer_allocation_table;

  /* Video information. */
  int lines, video_width_in_bytes;
  /* Framerate numerator and denominator */
  gint fps_n;
  gint fps_d;

  /* Centering offsets when playing video. */
  int cx, cy;

  GMutex flow_lock;
  GstBufferPool *pool;
  GstVideoInfo info;
  gboolean have_caps;
  GstCaps *caps;
  gboolean adjusted_dimensions;
  int adjusted_width;
  int adjusted_height;
};

struct _GstFramebufferSinkClass
{
  GstVideoSinkClass videosink_parent_class;

  gboolean (*open_hardware) (GstFramebufferSink *framebuffersink);
  void (*close_hardware) (GstFramebufferSink *framebuffersink);
  void (*get_supported_overlay_types) (GstFramebufferSink *framebuffersink, uint8_t *types);
  gboolean (*prepare_overlay) (GstFramebufferSink *framebuffersink, GstFramebufferSinkOverlayType t);
  GstFlowReturn (*show_overlay) (GstFramebufferSink *framebuffersink, guintptr framebuffer_offset);
};

GType gst_framebuffersink_get_type (void);

/* Allocator class. */

#define GST_TYPE_FRAMEBUFFERSINK_ALLOCATOR   (gst_framebuffersink_allocator_get_type())
#define GST_FRAMEBUFFERSINK_ALLOCATOR(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_FRAMEBUFFERSINK_ALLOCATOR,GstFramebufferSinkAllocator))
#define GST_FRAMEBUFFERSINK_ALLOCATOR_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_FRAMEBUFFERSINK_ALLOCATOR,GstFramebufferSinkAllocatorClass))
#define GST_IS_FRAMEBUFFERSINK_ALLOCATOR(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_FRAMEBUFFERSINK_ALLOCATOR))
#define GST_IS_FRAMEBUFFERSINK_ALLOCATOR_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_FRAMEBUFFERSINK_ALLOCATOR))

typedef struct _GstFramebufferSinkAllocatorClass GstFramebufferSinkAllocatorClass;

struct _GstFramebufferSinkAllocator {
  GstAllocator allocator;
  GstFramebufferSink *framebuffersink;
  int *buffers;
};

struct _GstFramebufferSinkAllocatorClass {
  GstAllocatorClass allocator_parent_class;
};

GType gst_framebuffersink_allocator_get_type (void);

G_END_DECLS

#endif
