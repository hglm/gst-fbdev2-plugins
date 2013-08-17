/* GStreamer GstFbdevFramebufferSink class
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

#ifndef _GST_FBDEVFRAMEBUFFERSINK_H_
#define _GST_FBDEVFRAMEBUFFERSINK_H_

#include <stdint.h>
#include <linux/fb.h>
#include <gst/video/gstvideosink.h>
#include <gst/video/video.h>
#include "gstframebuffersink.h"

G_BEGIN_DECLS

/* Main class. */

#define GST_TYPE_FBDEVFRAMEBUFFERSINK (gst_fbdevframebuffersink_get_type ())
#define GST_FBDEVFRAMEBUFFERSINK(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
    GST_TYPE_FBDEVFRAMEBUFFERSINK, GstFbdevFramebufferSink))
#define GST_FBDEVFRAMEBUFFERSINK_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST ((klass), \
    GST_TYPE_FBDEVFRAMEBUFFERSINK, GstFbdevFramebufferSinkClass))
#define GST_IS_FBDEVFRAMEBUFFERSINK(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
    GST_TYPE_FBDEVFRAMEBUFFERSINK))
#define GST_IS_FBDEVFRAMEBUFFERSINK_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_FBDEVFRAMEBUFFERSINK))
#define GST_FBDEVFRAMEBUFFERSINK_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_FRAMEBUFFER_SINK, \
     GstFbdevFramebufferSinkClass))

typedef struct _GstFbdevFramebufferSink GstFbdevFramebufferSink;
typedef struct _GstFbdevFramebufferSinkClass GstFbdevFramebufferSinkClass;

struct _GstFbdevFramebufferSink
{
  GstFramebufferSink framebuffersink;

  /* Properties. */
  gboolean use_graphics_mode;

  /* fbdev device parameters. */
  int fd;
  uint8_t *framebuffer;
  guintptr framebuffer_map_size;
  struct fb_fix_screeninfo fixinfo;
  struct fb_var_screeninfo varinfo;
  int saved_kd_mode;
};

struct _GstFbdevFramebufferSinkClass
{
  GstFramebufferSinkClass framebuffersink_parent_class;
};

GType gst_fbdevframebuffersink_get_type (void);

gboolean gst_fbdevframebuffersink_open_hardware (
    GstFramebufferSink *framebuffersink, GstVideoInfo *info,
    gsize *video_memory_size, gsize *pannable_video_memory_size);
void gst_fbdevframebuffersink_close_hardware (
    GstFramebufferSink *framebuffersink);

G_END_DECLS

#endif
