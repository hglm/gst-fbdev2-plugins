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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-fbdev2sink
 *
 * The fbdev2sink element implements an accelerated and optimized
 * video sink for the Linux console framebuffer. The basis of the
 * implementation is the optimized fbdev sink as implemented in the
 * GstFramebufferSink class.
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
 * gst-launch -v videotestsrc ! fbdev2sink >/dev/null
 * ]|
 * Output the video test signal to the framebuffer. The redirect to
 * null surpressed interference from console text mode.
 * |[
 * gst-launch -v videotestsrc ! fbdev2sink native-resolution=true
 * ]|
 * Run videotstsrc at native screen resolution
 * |[
 * gst-launch -v videotestsrc horizontal_speed=10 ! fbdev2sink \
 * native-resolution=true buffer-pool=true graphics-mode=true
 * ]|
 * This command illustrates some of the plugin's optimization features
 * by rendering to video memory with vsync and page flipping in
 * console graphics mode. There should be no tearing with page flipping/
 * vsync enabled. You might have to use the fps property to reduce the frame
 * rate on slower systems.
 * |[
 * gst-launch playbin uri=[uri] video-sink="fbdev2sink native-resolution=true"
 * ]|
 * Use playbin while passing options to fbdev2sink.
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
#include "gstfbdev2sink.h"

GST_DEBUG_CATEGORY_STATIC (gst_fbdev2sink_debug_category);
#define GST_CAT_DEFAULT gst_fbdev2sink_debug_category

/* Inline function to produce both normal message and debug info. */
static inline void GST_FBDEV2SINK_INFO_OBJECT (GstFbdev2sink *fbdev2sink,
const gchar *message) {
  if (!fbdev2sink->fbdevframebuffersink.framebuffersink.silent)
      g_print ("%s.\n", message);
  GST_INFO_OBJECT (fbdev2sink, message);
}

#define GST_FBDEV2SINK_TEMPLATE_CAPS \
        GST_VIDEO_CAPS_MAKE ("RGB") \
        "; " GST_VIDEO_CAPS_MAKE ("BGR") \
        "; " GST_VIDEO_CAPS_MAKE ("RGBx") \
        "; " GST_VIDEO_CAPS_MAKE ("BGRx") \
        "; " GST_VIDEO_CAPS_MAKE ("xRGB") \
        "; " GST_VIDEO_CAPS_MAKE ("xBGR") ", " \
        "framerate = (fraction) [ 0, MAX ], " \
        "width = (int) [ 1, MAX ], " "height = (int) [ 1, MAX ]"

static GstStaticPadTemplate gst_fbdev2sink_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_FBDEV2SINK_TEMPLATE_CAPS)
    );

/* Class initialization. */

#define gst_fbdev2sink_parent_class fbdevframebuffersink_parent_class
G_DEFINE_TYPE_WITH_CODE (GstFbdev2sink, gst_fbdev2sink, GST_TYPE_FBDEVFRAMEBUFFERSINK,
  GST_DEBUG_CATEGORY_INIT (gst_fbdev2sink_debug_category, "fbdev2sink", 0,
  "debug category for fbdev2sink element"));

static void
gst_fbdev2sink_class_init (GstFbdev2sinkClass* klass)
{
  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS(klass),
      gst_static_pad_template_get (&gst_fbdev2sink_sink_template));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS(klass),
      "Optimized Linux console framebuffer video sink",
      "Sink/Video",
      "fbdev2 framebuffer sink",
      "Harm Hanemaaijer <fgenfb@yahoo.com>");
}

/* Class member functions. */

static void
gst_fbdev2sink_init (GstFbdev2sink *fbdev2sink) {
  GstFramebufferSink *framebuffersink = GST_FRAMEBUFFERSINK (fbdev2sink);

  /* Override the default value of the hardware-overlay property from GstFramebufferSink. */
  framebuffersink->use_hardware_overlay = FALSE;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  /* Remember to set the rank if it's an element that is meant
     to be autoplugged by decodebin. */
  return gst_element_register (plugin, "fbdev2sink", GST_RANK_NONE,
      GST_TYPE_FBDEV2SINK);
}

/* these are normally defined by the GStreamer build system.
   If you are creating an element to be included in gst-plugins-*,
   remove these, as they're always defined.  Otherwise, edit as
   appropriate for your external plugin package. */
#ifndef VERSION
#define VERSION "0.1"
#endif
#ifndef PACKAGE
#define PACKAGE "gstfbdev2sink"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "gstreamer1.0-fbdev2-plugins"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/hglm"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    fbdev2sink,
    "Optimized Linux console framebuffer video sink",
    plugin_init, VERSION, "LGPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)
