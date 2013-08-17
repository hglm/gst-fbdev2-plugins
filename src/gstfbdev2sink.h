/* GStreamer fbdev2sink plugin
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

#ifndef _GST_FBDEV2SINK_H_
#define _GST_FBDEV2SINK_H_

#include <stdint.h>
#include <linux/fb.h>
#include "gstfbdevframebuffersink.h"

G_BEGIN_DECLS

/* Main class. */

#define GST_TYPE_FBDEV2SINK (gst_fbdev2sink_get_type ())
#define GST_FBDEV2SINK(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
    GST_TYPE_FBDEV2SINK, GstFbdev2sink))
#define GST_FBDEV2SINK_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), \
    GST_TYPE_FBDEV2SINK, GstFbdev2sinkClass))
#define GST_IS_FBDEV2SINK(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
    GST_TYPE_FBDEV2SINK))
#define GST_IS_FBDEV2SINK_CLASS(obj) (G_TYPE_CHECK_CLASS_TYPE ((klass), \
    GST_TYPE_FBDEV2SINK))

typedef struct _GstFbdev2sink GstFbdev2sink;
typedef struct _GstFbdev2sinkClass GstFbdev2sinkClass;

struct _GstFbdev2sink
{
  GstFbdevFramebufferSink fbdevframebuffersink;
};

struct _GstFbdev2sinkClass
{
  GstFbdevFramebufferSinkClass fbdevframebuffersink_parent_class;
};

GType gst_fbdev2sink_get_type (void);

G_END_DECLS

#endif
