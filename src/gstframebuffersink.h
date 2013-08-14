/* GStreamer GstFramebufferSink class
 * Copyright (C) 2013 Harm Hanemaaijer <fgenfb@yahoo.com>
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
  gchar *device;
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
  gboolean pan_does_vsync;
  gboolean preserve_par;
  gint max_video_memory_property;
  gchar *preferred_overlay_format_str;
  gboolean benchmark;

  /* Invariant device parameters. */
  GstVideoInfo screen_info;
  GstVideoFormat *overlay_formats_supported;
  gsize video_memory_size;
  gsize pannable_video_memory_size;
  int max_framebuffers;
  /* Variable device parameters. */
  int current_framebuffer_index;
  int current_overlay_index;
  int scaled_width, scaled_height;
  /* Overlay alignment restrictions. */
  int overlay_alignment;
  int overlay_scanline_alignment;
  int overlay_plane_alignment;
  gboolean overlay_scanline_alignment_is_fixed;
  /* Video memory allocation management. */
  GstAllocator *screen_video_memory_allocator;
  GstAllocationParams *screen_allocation_params;
  int nu_screens_used;
  GstMemory **screens;
  GstAllocator *overlay_video_memory_allocator;
  GstAllocationParams *overlay_allocation_params;
  int nu_overlays_used;
  GstMemory **overlays;

  /* Video information. */
  GstVideoInfo info;
  int clipped_height;
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

  GstBufferPool *pool;
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

  /* The open_hardware function should open the device and perform other initializations
   * if required. The function may call gst_framebuffersink_open_hardware_fbdev() for a
   * default fbdev hardware initialization. Should return TRUE on success, and fill in the
   * video info corresponding to the screen framebuffer format. */
  gboolean (*open_hardware) (GstFramebufferSink *framebuffersink, GstVideoInfo *info,
      gsize *video_memory_size, gsize *pannable_video_memory_size);
  void (*close_hardware) (GstFramebufferSink *framebuffersink);
  void (*pan_display) (GstFramebufferSink *framebuffersink, GstMemory *vmem);
  void (*wait_for_vsync) (GstFramebufferSink *framebuffersink);
  GstVideoFormat * (*get_supported_overlay_formats) (GstFramebufferSink *framebuffersink);
  void (*get_overlay_alignment_restrictions) (GstFramebufferSink *framebuffersink, GstVideoFormat format,
      int *overlay_alignment, int *overlay_scanline_alignment, int *overlay_plane_alignment,
      gboolean *overlay_scanline_alignment_is_fixed);
  gboolean (*prepare_overlay) (GstFramebufferSink *framebuffersink, GstVideoFormat format);
  GstFlowReturn (*show_overlay) (GstFramebufferSink *framebuffersink, GstMemory *memory);
  GstAllocator * (*video_memory_allocator_new) (GstFramebufferSink *framebuffersink,
      GstVideoInfo *info, gboolean pannable, gboolean is_overlay);
};

GType gst_framebuffersink_get_type (void);

#define GST_MEMORY_FLAG_VIDEO_MEMORY GST_MEMORY_FLAG_LAST

G_END_DECLS

#endif
