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

/* Forward declaration. */

typedef struct _GstFramebufferSinkAllocator GstFramebufferSinkAllocator;

/* Main class. */

#define GST_TYPE_FRAMEBUFFERSINK   (gst_framebuffersink_get_type())
#define GST_FRAMEBUFFERSINK(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_FRAMEBUFFERSINK,GstFramebufferSink))
#define GST_FRAMEBUFFERSINK_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_FRAMEBUFFERSINK,GstFramebufferSinkClass))
#define GST_IS_FRAMEBUFFERSINK(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_FRAMEBUFFERSINK))
#define GST_IS_FRAMEBUFFERSINK_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_FRAMEBUFFERSINK))
#define GST_FRAMEBUFFERSINK_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_FRAMEBUFFER_SINK, GstFramebufferSinkClass))

typedef struct _GstFramebufferSink GstFramebufferSink;
typedef struct _GstFramebufferSinkClass GstFramebufferSinkClass;

struct _GstFramebufferSink
{
  GstVideoSink videosink; /* Includes width and height. */

  /* Configurable properties. */
  gboolean silent;
  gboolean full_screen;
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
  gint max_video_memory_property;
  gchar *preferred_overlay_format_str;

  /* Invariant device parameters. */
  int fd;
  int saved_kd_mode;
  gchar *device;
  uint8_t *framebuffer;
  guintptr framebuffer_map_size;
  struct fb_fix_screeninfo fixinfo;
  int bytespp;
  uint32_t rmask, gmask, bmask;
  int endianness;
  GstVideoFormat framebuffer_format;
  int max_framebuffers;
  gboolean open_hardware_success;
  GstVideoFormat *overlay_formats_supported;
  /* Variable device parameters. */
  struct fb_var_screeninfo varinfo;
  int nu_framebuffers_used;
  int current_framebuffer_index;
  int nu_overlays_used;
  int current_overlay_index;
  int scaled_width, scaled_height;
  /* Overlay alignment restrictions. */
  int overlay_alignment;
  int overlay_scanline_alignment;
  int overlay_plane_alignment;
  gboolean overlay_scanline_alignment_is_fixed;
  /* Video memory allocation management. */
  GstAllocator *video_memory_allocator;
  GstAllocationParams *allocation_params;
  GstMemory **screens;
  GstMemory **overlays;

  /* Video information. */
  int lines;
  int framebuffer_video_width_in_bytes;
  /* Video width in bytes for each plane. */
  int source_video_width_in_bytes[4];
  /* Framerate numerator and denominator */
  gint fps_n;
  gint fps_d;
  /* Centering offsets when playing video. */
  int cx, cy;
  /* Actual overlay organization in video memory for each plane. */
  int overlay_plane_offset[4];
  int overlay_scanline_stride[4];
  int overlay_size;
  /* Whether the video format provided by GStreamer matches the native */
  /* alignment requirements. */
  gboolean overlay_alignment_is_native;

  GMutex flow_lock;
  GstBufferPool *pool;
  GstVideoInfo info;
  gboolean have_caps;
  GstCaps *caps;
  gboolean adjusted_dimensions;
  int adjusted_width;
  int adjusted_height;

  /* Stats. */
  int stats_video_frames_video_memory;
  int stats_video_frames_system_memory;
  int stats_overlay_frames_video_memory;
  int stats_overlay_frames_system_memory;
};

struct _GstFramebufferSinkClass
{
  GstVideoSinkClass videosink_parent_class;

  gboolean (*open_hardware) (GstFramebufferSink *framebuffersink);
  void (*close_hardware) (GstFramebufferSink *framebuffersink);
  GstVideoFormat * (*get_supported_overlay_formats) (GstFramebufferSink *framebuffersink);
  void (*get_alignment_restrictions) (GstFramebufferSink *framebuffersink, GstVideoFormat format,
      int *overlay_alignment, int *overlay_scanline_alignment, int *overlay_plane_alignment,
      gboolean *overlay_scanline_alignment_is_fixed);
  gboolean (*prepare_overlay) (GstFramebufferSink *framebuffersink, GstVideoFormat format);
  GstFlowReturn (*show_overlay) (GstFramebufferSink *framebuffersink, guintptr framebuffer_offset);
};

GType gst_framebuffersink_get_type (void);

/* Allocator class. */

#define GST_TYPE_FRAMEBUFFERSINK_ALLOCATOR   (gst_framebuffersink_allocator_get_type())
#define GST_FRAMEBUFFERSINK_ALLOCATOR(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_FRAMEBUFFERSINK_ALLOCATOR,GstFramebufferSinkAllocator))
#define GST_FRAMEBUFFERSINK_ALLOCATOR_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_FRAMEBUFFERSINK_ALLOCATOR,GstFramebufferSinkAllocatorClass))
#define GST_IS_FRAMEBUFFERSINK_ALLOCATOR(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_FRAMEBUFFERSINK_ALLOCATOR))
#define GST_IS_FRAMEBUFFERSINK_ALLOCATOR_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_FRAMEBUFFERSINK_ALLOCATOR))

typedef struct _GstFramebufferSinkAllocatorClass GstFramebufferSinkAllocatorClass;

struct _GstFramebufferSinkAllocator {
  GstAllocator allocator;
  GstFramebufferSink *framebuffersink;
  int *buffers;
  int nu_buffers;
};

struct _GstFramebufferSinkAllocatorClass {
  GstAllocatorClass allocator_parent_class;
};

GType gst_framebuffersink_allocator_get_type (void);

G_END_DECLS

#endif
