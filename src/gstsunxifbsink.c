/* GStreamer sunxifbsink plugin
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-sunxifbsink
 *
 * The sunxifbsink element intends implements a hardware accelerated
 * video sink for the console framebuffer of Allwinner A1x/A20-based
 * devices. The basis of the implementation is the optimized fbdev
 * sink as implemented in the GstFramebufferSink class.
 *
 * <refsect2>
 * <title>Property settings,<title>
 * <para>
 * The plugin comes with variety of configurable properties regulating
 * the size and frames per second of the video output, and various 
 * options regulating the rendering method (including rendering directly
 * to video memory and page flipping).
 * </para>
 * </refsect2>
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v videotestsrc ! sunxifbsink >/dev/null
 * ]|
 * Output the video test signal to the framebuffer. The redirect to
 * null surpressed interference from console text mode.
 * |[
 * gst-launch -v videotestsrc ! sunxifbsink native-resolution=true
 * ]|
 * Run videotstsrc at native screen resolution
 * |[
 * gst-launch -v videotestsrc horizontal_speed=10 ! sunxifbsink \
 * native-resolution=true buffer-pool=true graphics-mode=true
 * ]|
 * This command illustrates some of the plugin's optimization features
 * by rendering to video memory with vsync and page flipping in
 * console graphics mode. There should be no tearing with page flipping/
 * vsync enabled. You might have to use the fps property to reduce the frame
 * rate on slower systems.
 * |[
 * gst-launch playbin uri=[uri] video-sink="sunxifbsink native-resolution=true"
 * ]|
 * Use playbin while passing options to sunxifbsink.
 * </refsect2>
 * <refsect2>
 * <title>Caveats</title>
 * <para>
 * The actual implementation of the Linux framebuffer API varies between
 * systems, and methods beyond the most basic operating mode may not work
 * correctly on some systems. This primarily applies to page flipping
 * and vsync. The API implementation may be slower than expected on certain
 * hardware due to, for example, extra hidden vsyncs being performed in the
 * pan function. The "pan-does-vsync" option may help in that case.
 * </para>
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>
#include <linux/kd.h>
#include <glib/gprintf.h>

#include <gst/gst.h>
#include <gst/video/gstvideosink.h>
#include <gst/video/video.h>
#include <gst/video/video-info.h>
#include "gstsunxifbsink.h"

GST_DEBUG_CATEGORY_STATIC (gst_sunxifbsink_debug_category);
#define GST_CAT_DEFAULT gst_sunxifbsink_debug_category

/* Inline function to produce both normal message and debug info. */
static inline void GST_SUNXIFBSINK_INFO_OBJECT (GstSunxifbsink * sunxifbsink,
const gchar *message) {
  if (!sunxifbsink->framebuffersink.silent) g_print (message); g_print(".\n");
  GST_INFO_OBJECT (sunxifbsink, message);
}

/* Class function prototypes. */
static void gst_sunxifbsink_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_sunxifbsink_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);

enum
{
  PROP_0,
};

#define GST_SUNXIFBSINK_TEMPLATE_CAPS \
        GST_VIDEO_CAPS_MAKE ("RGB") \
        "; " GST_VIDEO_CAPS_MAKE ("BGR") \
        "; " GST_VIDEO_CAPS_MAKE ("RGBx") \
        "; " GST_VIDEO_CAPS_MAKE ("BGRx") \
        "; " GST_VIDEO_CAPS_MAKE ("xRGB") \
        "; " GST_VIDEO_CAPS_MAKE ("xBGR") ", " \
        "framerate = (fraction) [ 0, MAX ], " \
        "width = (int) [ 1, MAX ], " "height = (int) [ 1, MAX ]"


static GstStaticPadTemplate gst_sunxifbsink_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_SUNXIFBSINK_TEMPLATE_CAPS)
    );

/* Class initialization. */

#define gst_sunxifbsink_parent_class videosink_parent_class
G_DEFINE_TYPE_WITH_CODE (GstSunxifbsink, gst_sunxifbsink, GST_TYPE_FRAMEBUFFERSINK,
  GST_DEBUG_CATEGORY_INIT (gst_sunxifbsink_debug_category, "sunxifbsink", 0,
  "debug category for sunxifbsink element"));

static void
gst_sunxifbsink_class_init (GstSunxifbsinkClass* klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseSinkClass *base_sink_class = GST_BASE_SINK_CLASS (klass);
  GstVideoSinkClass *video_sink_class = (GstVideoSinkClass *) klass;
  GstFramebufferSinkClass *framebuffer_sink_clas = GST_FRAMEBUFFERSINK_CLASS (klass);

//  gobject_class->set_property = gst_sunxifbsink_set_property;
//  gobject_class->get_property = gst_sunxifbsink_get_property;

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS(klass),
      gst_static_pad_template_get (&gst_sunxifbsink_sink_template));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS(klass),
      "Accelerated console framebuffer video sink for sunxi-based devices",
      "Sink/Video",
      "sunxi framebuffer sink",
      "Harm Hanemaaijer <fgenfb@yahoo.com>");
}

/* Class member functions. */

static void
gst_sunxifbsink_init (GstSunxifbsink *sunxifbsink) {
}

void
gst_sunxifbsink_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSunxifbsink *sunxifbsink = GST_SUNXIFBSINK (object);

  GST_DEBUG_OBJECT (sunxifbsink, "set_property");
  g_return_if_fail (GST_IS_SUNXIFBSINK (object));
}

static void
gst_sunxifbsink_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstSunxifbsink *sunxifbsink = GST_SUNXIFBSINK (object);

  GST_DEBUG_OBJECT (sunxifbsink, "get_property");
  g_return_if_fail (GST_IS_SUNXIFBSINK (object));
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  /* Remember to set the rank if it's an element that is meant
     to be autoplugged by decodebin. */
  return gst_element_register (plugin, "sunxifbsink", GST_RANK_NONE,
      GST_TYPE_SUNXIFBSINK);
}

/* these are normally defined by the GStreamer build system.
   If you are creating an element to be included in gst-plugins-*,
   remove these, as they're always defined.  Otherwise, edit as
   appropriate for your external plugin package. */
#ifndef VERSION
#define VERSION "0.1"
#endif
#ifndef PACKAGE
#define PACKAGE "gstsunxifbsink"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "gstreamer1.0-sunxi-plugins"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/hglm"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    sunxifbsink,
    "Accelerated console framebuffer video sink for sunxi-based devices",
    plugin_init, VERSION, "LGPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)

