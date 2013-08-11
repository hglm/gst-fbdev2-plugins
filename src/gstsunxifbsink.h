/* GStreamer sunxifbsink plugin
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

#ifndef _GST_SUNXIFBSINK_H_
#define _GST_SUNXIFBSINK_H_

#include <stdint.h>
#include <linux/fb.h>
#include "sunxi_disp_ioctl.h"
#include "gstfbdevframebuffersink.h"

G_BEGIN_DECLS

/* Forward declaration. */

typedef struct _GstSunxifbsinkAllocator GstSunxifbsinkAllocator;

/* Main class. */

#define GST_TYPE_SUNXIFBSINK   (gst_sunxifbsink_get_type())
#define GST_SUNXIFBSINK(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_SUNXIFBSINK,GstSunxifbsink))
#define GST_SUNXIFBSINK_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_SUNXIFBSINK,GstSunxifbsinkClass))
#define GST_IS_SUNXIFBSINK(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_SUNXIFBSINK))
#define GST_IS_SUNXIFBSINK_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_SUNXIFBSINK))

typedef struct _GstSunxifbsink GstSunxifbsink;
typedef struct _GstSunxifbsinkClass GstSunxifbsinkClass;

struct _GstSunxifbsink
{
  GstFbdevFramebufferSink fbdevframebuffersink;
  gboolean hardware_overlay_available;
  int fd_disp;
  int framebuffer_id;
  int gfx_layer_id;
  int layer_id;
  gboolean layer_has_scaler;
  gboolean layer_is_visible;
  GstVideoFormat overlay_format;
};

struct _GstSunxifbsinkClass
{
  GstFbdevFramebufferSinkClass fbdevframebuffersink_parent_class;
};

GType gst_sunxifbsink_get_type (void);

G_END_DECLS

#endif
