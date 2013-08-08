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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:class-GstFramebufferSink
 *
 * The GstFramebufferSink class implements an optimized video sink
 * for the Linux console framebuffer. It is used as the basis for the
 * fbdev2sink plugin. It can write directly into video memory with
 * page flipping support, and should be usable by a wide variety of
 * devices. The class can be derived for device-specific implementations
 * with hardware acceleration.
 *
 * <refsect2>
 * <title>Property settings,<title>
 * <para>
 * The class comes with variety of configurable properties regulating
 * the size and frames per second of the video output, and various 
 * options regulating the rendering method (including rendering directly
 * to video memory and page flipping).
 * </para>
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
#include <gst/video/gstvideopool.h>
#include <gst/video/video.h>
#include <gst/video/video-info.h>
#include <gst/video/gstvideometa.h>
#include "gstframebuffersink.h"

GST_DEBUG_CATEGORY_STATIC (gst_framebuffersink_debug_category);
#define GST_CAT_DEFAULT gst_framebuffersink_debug_category

// #define EXTRA_DEBUG

/* Definitions to influence buffer pool allocation. */
/* Provide the same pool for repeated requests. */
// #define USE_SAME_POOL
/* Provide another video memory pool for repeated requests. */
#define MULTIPLE_VIDEO_MEMORY_POOLS
/* Provide half of the available video memory pool buffer per request. */
#define HALF_POOLS


/* Function to produce both normal message and debug info. */
static void GST_FRAMEBUFFERSINK_INFO_OBJECT (GstFramebufferSink * framebuffersink,
const gchar *message) {
  if (!framebuffersink->silent) {
    g_print (message);
    g_print(".\n");
  }
  GST_INFO_OBJECT (framebuffersink, message);
}

#define ALIGNMENT_GET_ALIGN_BYTES(offset, align) \
    (((align) + 1 - ((offset) & (align))) & (align))
#define ALIGNMENT_GET_ALIGNED(offset, align) \
    ((offset) + ALIGNMENT_GET_ALIGN_BYTES(offset, align))
#define ALIGNMENT_APPLY(offset, align) \
    offset = ALIGNMENT_GET_ALIGNED(offset, align);

/* Class function prototypes. */
static void gst_framebuffersink_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_framebuffersink_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static GstCaps *gst_framebuffersink_get_caps (GstBaseSink * sink, GstCaps * filter);
static gboolean gst_framebuffersink_set_caps (GstBaseSink * sink, GstCaps * caps);
static gboolean gst_framebuffersink_start (GstBaseSink * sink);
static gboolean gst_framebuffersink_stop (GstBaseSink * sink);
static GstFlowReturn gst_framebuffersink_show_frame (GstVideoSink * vsink, GstBuffer * buf);
static gboolean gst_framebuffersink_propose_allocation (GstBaseSink * sink, GstQuery * query);

/* Defaults for virtual functions defined in this class. */
static GstVideoFormat *gst_framebuffersink_get_supported_overlay_formats (GstFramebufferSink *framebuffersink);
static gboolean gst_framebuffersink_open_hardware (GstFramebufferSink *framebuffersink);
static void gst_framebuffersink_close_hardware (GstFramebufferSink *framebuffersink);

/* Local functions. */
static gboolean gst_framebuffersink_open_device (GstFramebufferSink * sink);
static void gst_buffer_print(GstFramebufferSink *framebuffersink, GstBuffer *buf);

/* Video memory. */
static void gst_framebuffersink_video_memory_init (gpointer framebuffer, gsize map_size);
static GstMemory * gst_framebuffersink_video_memory_alloc (gsize size, int align);
static void gst_framebuffersink_video_memory_free (GstMemory *mem);
static gboolean gst_framebuffersink_is_video_memory (GstMemory *mem);
static gsize gst_framebuffersink_video_memory_get_available (void);

enum
{
  PROP_0,
  PROP_SILENT,
  PROP_DEVICE,
  PROP_ACTUAL_WIDTH,
  PROP_ACTUAL_HEIGHT,
  PROP_REQUESTED_WIDTH,
  PROP_REQUESTED_HEIGHT,
  PROP_SCREEN_WIDTH,
  PROP_SCREEN_HEIGHT,
  PROP_WIDTH_BEFORE_SCALING,
  PROP_HEIGHT_BEFORE_SCALING,
  PROP_FULL_SCREEN,
  PROP_PRESERVE_PAR,
  PROP_CLEAR,
  PROP_FRAMES_PER_SECOND,
  PROP_BUFFER_POOL,
  PROP_VSYNC,
  PROP_FLIP_BUFFERS,
  PROP_GRAPHICS_MODE,
  PROP_PAN_DOES_VSYNC,
  PROP_USE_HARDWARE_OVERLAY,
  PROP_MAX_VIDEO_MEMORY_USED,
  PROP_OVERLAY_FORMAT,
};

/* pad templates */

#define GST_FRAMEBUFFERSINK_TEMPLATE_CAPS \
        GST_VIDEO_CAPS_MAKE ("RGB") \
        "; " GST_VIDEO_CAPS_MAKE ("BGR") \
        "; " GST_VIDEO_CAPS_MAKE ("RGBx") \
        "; " GST_VIDEO_CAPS_MAKE ("BGRx") \
        "; " GST_VIDEO_CAPS_MAKE ("xRGB") \
        "; " GST_VIDEO_CAPS_MAKE ("xBGR") ", " \
        "framerate = (fraction) [ 0, MAX ], " \
        "width = (int) [ 1, MAX ], " "height = (int) [ 1, MAX ]"


static GstStaticPadTemplate gst_framebuffersink_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_FRAMEBUFFERSINK_TEMPLATE_CAPS)
    );

static GstVideoFormat overlay_formats_supported_table_empty[] = {
  GST_VIDEO_FORMAT_UNKNOWN
};

/* Class initialization. */

static void
gst_framebuffersink_class_init (GstFramebufferSinkClass* klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseSinkClass *base_sink_class = GST_BASE_SINK_CLASS (klass);
  GstVideoSinkClass *video_sink_class = GST_VIDEO_SINK_CLASS (klass);

  gobject_class->set_property = gst_framebuffersink_set_property;
  gobject_class->get_property = gst_framebuffersink_get_property;

  /* define properties */
  g_object_class_install_property (gobject_class, PROP_SILENT,
    g_param_spec_boolean ("silent", "Reduce messages",
			  "Whether to be very verbose or not",
			  FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_DEVICE,
      g_param_spec_string ("device", "The framebuffer device",
          "The framebuffer device", "/dev/fb0",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_ACTUAL_WIDTH,
    g_param_spec_int ("actual-width", "Actual source video width",
			  "Actual width of the video window source", 0, G_MAXINT,
			  0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_ACTUAL_HEIGHT,
    g_param_spec_int ("actual-height", "Actual source video height",
			  "Actual height of the video window source", 0, G_MAXINT,
			  0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_REQUESTED_WIDTH,
    g_param_spec_int ("width", "Requested width",
			  "Requested width of the video output window (0 = auto)",
                          0, G_MAXINT,
			  0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_REQUESTED_HEIGHT,
    g_param_spec_int ("height", "Requested height",
			  "Requested height of the video output window (0 = auto)",
                          0, G_MAXINT,
			  0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_SCREEN_WIDTH,
    g_param_spec_int ("screen-width", "Screen width",
			  "Width of the screen", 1, G_MAXINT,
			  1, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_SCREEN_HEIGHT,
    g_param_spec_int ("screen-height", "Screen height",
			  "Height of the screen", 1, G_MAXINT,
			  1, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_WIDTH_BEFORE_SCALING,
    g_param_spec_int ("width-before-scaling", "Requested source width before scaling",
			  "Requested width of the video source when using hardware scaling "
                          "(0 = use default source width)",
                          0, G_MAXINT,
			  0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_HEIGHT_BEFORE_SCALING,
    g_param_spec_int ("height-before-scaling", "Requested source height before scaling",
			  "Requested height of the video source when using hardware scaling "
                          "(0 = use default source height)",
                          0, G_MAXINT,
			  0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_FULL_SCREEN,
    g_param_spec_boolean ("full-screen", "Full-screen output",
			  "Force full-screen video output resolution "
                          "(equivalent to setting width and "
                          "height to screen dimensions)",
                          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PRESERVE_PAR,
    g_param_spec_boolean ("preserve-par", "Preserve pixel aspect ratio",
			  "Preserve the pixel aspect ratio by adding black boxes if necessary",
                          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_CLEAR,
    g_param_spec_boolean ("clear", "Clear the screen",
			  "Clear the screen to black before playing",
                          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_FRAMES_PER_SECOND,
    g_param_spec_int ("fps", "Frames per second",
			  "Frames per second (0 = auto)", 0, G_MAXINT,
			  0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_BUFFER_POOL,
    g_param_spec_boolean ("buffer-pool", "Use buffer pool",
			  "Use a custom buffer pool in video memory and write directly to the "
                          "screen if possible",
                          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_VSYNC,
    g_param_spec_boolean ("vsync", "VSync",
			  "Sync to vertical retrace. Especially useful with buffer-pool=true.",
                          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_FLIP_BUFFERS,
    g_param_spec_int ("flip-buffers", "Max number of page-flip buffers",
			  "The maximum number of buffers in video memory to use for page flipping. "
                          "Page flipping is disabled when set to 1. Use of a buffer-pool requires "
                          "at least 2 buffers. Default is 0 (auto).",
                          0, G_MAXINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_GRAPHICS_MODE,
    g_param_spec_boolean ("graphics-mode", "Console graphics mode",
			  "Set the console to KDGRAPHICS mode. This eliminates interference from "
                          "text output and the cursor but can result in textmode not being restored "
                          "in case of a crash. Use with care.",
                          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PAN_DOES_VSYNC,
    g_param_spec_boolean ("pan-does-vsync", "Pan does vsync indicator",
			  "When set to true this property hints that the kernel display pan function "
                          "performs vsync automatically or otherwise doesn't need a vsync call "
                          "around it.",
                          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_USE_HARDWARE_OVERLAY,
    g_param_spec_boolean ("hardware-overlay", "Use hardware overlay",
                          "Use hardware overlay scaler if available. Not available in the default "
                          "fbdev2sink but may be available in derived sinks.",
                          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_MAX_VIDEO_MEMORY_USED,
    g_param_spec_int ("video-memory", "Max video memory used in MB",
                          "The maximum amount of video memory to use in MB. Three special values "
                          "are defined: 0 (the default) limits the amount to the virtual resolution "
                          "as reported by the Linux fb interface; -1 uses up to all available video "
                          "memory as reported by the fb interface but sets sane limits; -2 aggressively "
                          "uses all available memory.",
                          -2, G_MAXINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_OVERLAY_FORMAT,
      g_param_spec_string ("overlay-format", "Overlay format",
          "Set the preferred overlay format (four character code); by default the standard rank order "
          "provided by the plugin will be applied", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  base_sink_class->start = GST_DEBUG_FUNCPTR (gst_framebuffersink_start);
  base_sink_class->stop = GST_DEBUG_FUNCPTR (gst_framebuffersink_stop);
  base_sink_class->get_caps = GST_DEBUG_FUNCPTR (gst_framebuffersink_get_caps);
  base_sink_class->set_caps = GST_DEBUG_FUNCPTR (gst_framebuffersink_set_caps);
  base_sink_class->propose_allocation = GST_DEBUG_FUNCPTR (gst_framebuffersink_propose_allocation);
  video_sink_class->show_frame = GST_DEBUG_FUNCPTR (gst_framebuffersink_show_frame);
  klass->open_hardware = GST_DEBUG_FUNCPTR (gst_framebuffersink_open_hardware);
  klass->close_hardware = GST_DEBUG_FUNCPTR (gst_framebuffersink_close_hardware);
  klass->get_supported_overlay_formats = GST_DEBUG_FUNCPTR (gst_framebuffersink_get_supported_overlay_formats);
}

static void
gst_framebuffersink_base_init (gpointer g_class)
{
  GST_DEBUG_CATEGORY_INIT (gst_framebuffersink_debug_category,
      "framebuffersink", 0, "GstFramebufferSink" );
}

/* Class member functions. */

static void
gst_framebuffersink_init (GstFramebufferSink *framebuffersink) {
  framebuffersink->framebuffer = NULL;
  framebuffersink->device = NULL;
  framebuffersink->pool = NULL;
  framebuffersink->have_caps = FALSE;
  framebuffersink->adjusted_dimensions = FALSE;

  /* Set the initial values of the properties.*/
  framebuffersink->device = g_strdup("/dev/fb0");
  framebuffersink->videosink.width = 0;
  framebuffersink->videosink.height = 0;
  framebuffersink->silent = FALSE;
  framebuffersink->full_screen = FALSE;
  framebuffersink->requested_video_width = 0;
  framebuffersink->requested_video_height = 0;
  framebuffersink->width_before_scaling = 0;
  framebuffersink->height_before_scaling = 0;
  framebuffersink->varinfo.xres = 1;
  framebuffersink->varinfo.yres = 1;
  framebuffersink->clear = TRUE;
  framebuffersink->fps = 0;
  framebuffersink->use_buffer_pool = FALSE;
  framebuffersink->vsync = TRUE;
  framebuffersink->flip_buffers = 0;
  framebuffersink->use_graphics_mode = FALSE;
  framebuffersink->pan_does_vsync = FALSE;
  framebuffersink->use_hardware_overlay = TRUE;
  framebuffersink->preserve_par = TRUE;
  framebuffersink->max_video_memory_property = 0;
  framebuffersink->preferred_overlay_format_str = NULL;
  gst_video_info_init (&framebuffersink->info);
}

/* Default implementation of hardware open/close functions: do nothing. */

static gboolean
gst_framebuffersink_open_hardware (GstFramebufferSink *framebuffersink) {
  return TRUE;
}

static void
  gst_framebuffersink_close_hardware (GstFramebufferSink *framebuffersink) {
}

/* Default implementation of get_supported_overlay_formats: none supported. */

static GstVideoFormat *
gst_framebuffersink_get_supported_overlay_formats (GstFramebufferSink *framebuffersink)
{
  return overlay_formats_supported_table_empty;
}

static gboolean
gst_framebuffersink_video_format_supported_by_overlay (GstFramebufferSink *framebuffersink, GstVideoFormat format) {
  GstVideoFormat *f = framebuffersink->overlay_formats_supported;
  while (*f != GST_VIDEO_FORMAT_UNKNOWN) {
    if (*f == format)
      return TRUE;
    f++;
  }
  return FALSE;
}

static int
gst_framebuffersink_get_overlay_format_rank (GstFramebufferSink *framebuffersink, GstVideoFormat format)
{
  GstVideoFormat *f = framebuffersink->overlay_formats_supported;
  int r = 0;
  while (*f != GST_VIDEO_FORMAT_UNKNOWN) {
    if (*f == format)
      return r;
    f++;
    r++;
  }
  return G_MAXINT;
}

static void
gst_framebuffersink_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstFramebufferSink *framebuffersink = GST_FRAMEBUFFERSINK (object);

  GST_DEBUG_OBJECT (framebuffersink, "set_property");
  g_return_if_fail (GST_IS_FRAMEBUFFERSINK (object));

  switch (property_id) {
    case PROP_SILENT:
      framebuffersink->silent = g_value_get_boolean (value);
      break;
    case PROP_DEVICE:
      g_free (framebuffersink->device);
      framebuffersink->device = g_value_dup_string (value);
      break;
    case PROP_REQUESTED_WIDTH:
      framebuffersink->requested_video_width = g_value_get_int (value);
      break;
    case PROP_REQUESTED_HEIGHT:
      framebuffersink->requested_video_height = g_value_get_int (value);
      break;
    case PROP_WIDTH_BEFORE_SCALING:
      framebuffersink->width_before_scaling = g_value_get_int (value);
      break;
    case PROP_HEIGHT_BEFORE_SCALING:
      framebuffersink->height_before_scaling = g_value_get_int (value);
      break;
    case PROP_FULL_SCREEN:
      framebuffersink->full_screen = g_value_get_boolean (value);
      break;
    case PROP_PRESERVE_PAR:
      framebuffersink->preserve_par = g_value_get_boolean (value);
      break;
    case PROP_CLEAR:
      framebuffersink->clear = g_value_get_boolean (value);
      break;
    case PROP_FRAMES_PER_SECOND:
      framebuffersink->fps = g_value_get_int (value);
      break;
    case PROP_BUFFER_POOL:
      framebuffersink->use_buffer_pool = g_value_get_boolean (value);
      break;
    case PROP_VSYNC:
      framebuffersink->vsync = g_value_get_boolean (value);
      break;
    case PROP_FLIP_BUFFERS:
      framebuffersink->flip_buffers = g_value_get_int (value);
      break;
    case PROP_GRAPHICS_MODE:
      framebuffersink->use_graphics_mode = g_value_get_boolean (value);
      break;
    case PROP_PAN_DOES_VSYNC:
      framebuffersink->pan_does_vsync = g_value_get_boolean (value);
      break;
    case PROP_USE_HARDWARE_OVERLAY:
      framebuffersink->use_hardware_overlay = g_value_get_boolean (value);
      break;
    case PROP_MAX_VIDEO_MEMORY_USED:
      framebuffersink->max_video_memory_property = g_value_get_int (value);
      break;
    case PROP_OVERLAY_FORMAT:
      if (framebuffersink->preferred_overlay_format_str != NULL)
        g_free (framebuffersink->preferred_overlay_format_str);
      framebuffersink->preferred_overlay_format_str = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gst_framebuffersink_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstFramebufferSink *framebuffersink = GST_FRAMEBUFFERSINK (object);

  GST_DEBUG_OBJECT (framebuffersink, "get_property");
  g_return_if_fail (GST_IS_FRAMEBUFFERSINK (object));

  switch (property_id) {
    case PROP_SILENT:
      g_value_set_boolean (value, framebuffersink->silent);
      break;
    case PROP_DEVICE:
      g_value_set_string (value, framebuffersink->device);
      break;
    case PROP_ACTUAL_WIDTH:
      g_value_set_int (value, framebuffersink->videosink.width);
      break;
    case PROP_ACTUAL_HEIGHT:
      g_value_set_int (value, framebuffersink->videosink.height);
      break;
    case PROP_REQUESTED_WIDTH:
      g_value_set_int (value, framebuffersink->requested_video_width);
      break;
    case PROP_REQUESTED_HEIGHT:
      g_value_set_int (value, framebuffersink->requested_video_height);
      break;
    case PROP_SCREEN_WIDTH:
      g_value_set_int (value, framebuffersink->varinfo.xres);
      break;
    case PROP_SCREEN_HEIGHT:
      g_value_set_int (value, framebuffersink->varinfo.yres);
      break;
    case PROP_WIDTH_BEFORE_SCALING:
      g_value_set_int (value, framebuffersink->width_before_scaling);
      break;
    case PROP_HEIGHT_BEFORE_SCALING:
      g_value_set_int (value, framebuffersink->height_before_scaling);
      break;
    case PROP_FULL_SCREEN:
      g_value_set_boolean (value, framebuffersink->full_screen);
      break;
    case PROP_PRESERVE_PAR:
      g_value_set_boolean (value, framebuffersink->preserve_par);
      break;
    case PROP_CLEAR:
      g_value_set_boolean (value, framebuffersink->clear);
      break;
    case PROP_FRAMES_PER_SECOND:
      g_value_set_int (value, framebuffersink->fps);
      break;
    case PROP_BUFFER_POOL:
      g_value_set_boolean (value, framebuffersink->use_buffer_pool);
      break;
    case PROP_VSYNC:
      g_value_set_boolean (value, framebuffersink->vsync);
      break;
    case PROP_FLIP_BUFFERS:
      g_value_set_int (value, framebuffersink->flip_buffers);
      break;
    case PROP_GRAPHICS_MODE:
      g_value_set_boolean (value, framebuffersink->use_graphics_mode);
      break;
    case PROP_PAN_DOES_VSYNC:
      g_value_set_boolean (value, framebuffersink->pan_does_vsync);
      break;
    case PROP_USE_HARDWARE_OVERLAY:
      g_value_set_boolean (value, framebuffersink->use_hardware_overlay);
      break;
    case PROP_MAX_VIDEO_MEMORY_USED:
      g_value_set_int (value, framebuffersink->max_video_memory_property);
      break;
    case PROP_OVERLAY_FORMAT:
      g_value_set_string (value, framebuffersink->preferred_overlay_format_str);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static uint32_t
swapendian (uint32_t val)
{
  return (val & 0xff) << 24 | (val & 0xff00) << 8
      | (val & 0xff0000) >> 8 | (val & 0xff000000) >> 24;
}

static gboolean
gst_framebuffersink_open_device(GstFramebufferSink *framebuffersink) {
  uint32_t rmask;
  uint32_t gmask;
  uint32_t bmask;
  int endianness;
  int depth;

  if (!framebuffersink->device) {
    framebuffersink->device = g_strdup ("/dev/fb0");
  }

  framebuffersink->fd = open (framebuffersink->device, O_RDWR);

  if (framebuffersink->fd == -1)
    goto err;

  /* get the fixed screen info */
  if (ioctl (framebuffersink->fd, FBIOGET_FSCREENINFO, &framebuffersink->fixinfo))
    goto err;

  /* get the variable screen info */
  if (ioctl (framebuffersink->fd, FBIOGET_VSCREENINFO, &framebuffersink->varinfo))
    goto err;

  /* Map the framebuffer. */
  if (framebuffersink->max_video_memory_property == 0)
    /* Only allocate up to reported virtual size when the video-memory property is 0. */
    framebuffersink->framebuffer_map_size =
      framebuffersink->fixinfo.line_length * framebuffersink->varinfo.yres_virtual;
  else if (framebuffersink->max_video_memory_property == - 1) {
    /* Allocate up to 8 screens when the property is set to - 1. */
    framebuffersink->framebuffer_map_size =
        framebuffersink->fixinfo.line_length * framebuffersink->varinfo.yres * 8;
    if (framebuffersink->framebuffer_map_size > framebuffersink->fixinfo.smem_len)
      framebuffersink->framebuffer_map_size = framebuffersink->fixinfo.smem_len;
  }
  else if (framebuffersink->max_video_memory_property == - 2)
    /* Allocate all video memorywhen video-memory is set to - 2. */
    framebuffersink->framebuffer_map_size = framebuffersink->fixinfo.smem_len;
  else {
     /* Use the setting from video-memory, but sanitize it. */
    framebuffersink->framebuffer_map_size =
        framebuffersink->max_video_memory_property * 1024 * 1024;
    if (framebuffersink->framebuffer_map_size > framebuffersink->fixinfo.smem_len)
      framebuffersink->framebuffer_map_size = framebuffersink->fixinfo.smem_len;
    if (framebuffersink->framebuffer_map_size < framebuffersink->fixinfo.line_length
        * framebuffersink->varinfo.yres)
      framebuffersink->framebuffer_map_size = framebuffersink->fixinfo.line_length
          * framebuffersink->varinfo.yres;
  }
  framebuffersink->framebuffer = mmap (0, framebuffersink->framebuffer_map_size,
      PROT_WRITE, MAP_SHARED, framebuffersink->fd, 0);
  if (framebuffersink->framebuffer == MAP_FAILED)
    goto err;

  framebuffersink->max_framebuffers = framebuffersink->framebuffer_map_size /
      (framebuffersink->fixinfo.line_length * framebuffersink->varinfo.yres);

  framebuffersink->nu_framebuffers_used = 1;

  /* Check the pixel depth and determine the color masks. */
  rmask = ((1 << framebuffersink->varinfo.red.length) - 1)
      << framebuffersink->varinfo.red.offset;
  gmask = ((1 << framebuffersink->varinfo.green.length) - 1)
      << framebuffersink->varinfo.green.offset;
  bmask = ((1 << framebuffersink->varinfo.blue.length) - 1)
      << framebuffersink->varinfo.blue.offset;
  endianness = 0;

  switch (framebuffersink->varinfo.bits_per_pixel) {
    case 32:
      /* swap endian of masks */
      rmask = swapendian (rmask);
      gmask = swapendian (gmask);
      bmask = swapendian (bmask);
      endianness = 4321;
      break;
    case 24: {
      /* swap red and blue masks */
      uint32_t t = rmask;
      rmask = bmask;
      bmask = t;
      endianness = 4321;
      break;
      }
    case 15:
    case 16:
      endianness = 1234;
      break;
    default:
      /* other bit depths are not supported */
      GST_ERROR ("unsupported bit depth: %d\n",
      framebuffersink->varinfo.bits_per_pixel);
      goto err;
  }

  framebuffersink->rmask = rmask;
  framebuffersink->gmask = gmask;
  framebuffersink->bmask = bmask;
  framebuffersink->endianness = endianness;
  framebuffersink->bytespp = (framebuffersink->varinfo.bits_per_pixel + 7) / 8;

  /* Set the framebuffer video format. */
  depth = framebuffersink->varinfo.red.length + framebuffersink->varinfo.green.length
      + framebuffersink->varinfo.blue.length;

  framebuffersink->framebuffer_format = gst_video_format_from_masks (depth, framebuffersink->varinfo.bits_per_pixel,
      framebuffersink->endianness, framebuffersink->rmask, framebuffersink->gmask, framebuffersink->bmask, 0);

  char *s = malloc(strlen(framebuffersink->device) + 256);
  g_sprintf(s, "Succesfully opened framebuffer device %s, "
      "pixel depth %d, dimensions %d x %d, mapped sized %d MB "
      "of which %d MB (%d buffers) usable for page flipping",
      framebuffersink->device,
      framebuffersink->bytespp * 8, framebuffersink->varinfo.xres,
      framebuffersink->varinfo.yres, framebuffersink->framebuffer_map_size / (1024 * 1024),
      framebuffersink->max_framebuffers * framebuffersink->fixinfo.line_length *
      framebuffersink->varinfo.yres / (1024 * 1024), framebuffersink->max_framebuffers);
  if (framebuffersink->vsync)
    g_sprintf(s + strlen(s), ", vsync enabled");
  GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink, s);
  free(s);

  g_mutex_init (&framebuffersink->flow_lock);

  if (framebuffersink->use_graphics_mode) {
    int kd_fd;
    kd_fd = open ("/dev/tty0", O_RDWR);
    if (kd_fd < 0)
        goto error_setting_graphics_mode;
    if (ioctl (kd_fd, KDGETMODE, &framebuffersink->saved_kd_mode) < 0)
        goto error_setting_graphics_mode;
    if (ioctl (kd_fd, KDSETMODE, KD_GRAPHICS) < 0)
        goto error_setting_graphics_mode;
    GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, "Setting console to KD_GRAPHICS mode");
    close (kd_fd);
  }

  gst_framebuffersink_video_memory_init (framebuffersink->framebuffer,
      framebuffersink->framebuffer_map_size);
  framebuffersink->video_memory_allocator = gst_allocator_find ("framebuffersink_video_memory");

  return TRUE;

err:
  GST_ERROR_OBJECT (framebuffersink, "Could not initialise framebuffer output");
  return FALSE;

error_setting_graphics_mode:
  GST_WARNING_OBJECT (framebuffersink, "Could not set KD mode to KD_GRAPHICS");
  framebuffersink->use_graphics_mode = FALSE;
  return TRUE;

}

static gboolean
gst_framebuffersink_set_device_virtual_size(GstFramebufferSink *framebuffersink, int xres, int yres)
{
  framebuffersink->varinfo.xres_virtual = xres;
  framebuffersink->varinfo.yres_virtual = yres;
  /* Set the variable screen info. */
  if (ioctl (framebuffersink->fd, FBIOPUT_VSCREENINFO, &framebuffersink->varinfo))
    return FALSE;
  /* Read back test. */
  ioctl (framebuffersink->fd, FBIOGET_VSCREENINFO, &framebuffersink->varinfo);
  if (framebuffersink->varinfo.yres_virtual != yres)
    return FALSE;
  return TRUE;
}

static void
gst_framebuffersink_clear_screen (GstFramebufferSink *framebuffersink, int index) {
  GstMapInfo mapinfo;
  gst_memory_map (framebuffersink->screens[index], &mapinfo, GST_MAP_WRITE);
  memset (mapinfo.data, 0, mapinfo.size);
  gst_memory_unmap (framebuffersink->screens[index], &mapinfo);
}

static void
gst_framebuffersink_put_image_memcpy (GstFramebufferSink *framebuffersink, uint8_t * src)
{
  guint8 *dest;
  guintptr dest_stride;
  int i;
  GstMapInfo mapinfo;

  if (framebuffersink->use_buffer_pool)
    /* Hack: when using a buffer pool in video memory, and a system memory buffer is */
    /* inadvertenty provided, just copy to the first screen. */
    dest = framebuffersink->framebuffer;
  else {
    gst_memory_map (framebuffersink->screens[framebuffersink->current_framebuffer_index], &mapinfo, GST_MAP_WRITE);
    dest = mapinfo.data;
  }
  dest += framebuffersink->cy * framebuffersink->fixinfo.line_length + framebuffersink->cx
      * framebuffersink->bytespp;
  dest_stride = framebuffersink->fixinfo.line_length;
  if (framebuffersink->framebuffer_video_width_in_bytes == dest_stride)
      memcpy (dest, src, dest_stride * framebuffersink->lines);
  else
    for (i = 0; i < framebuffersink->lines; i++) {
      memcpy (dest, src, framebuffersink->framebuffer_video_width_in_bytes);
      src += framebuffersink->source_video_width_in_bytes[0];
      dest += dest_stride;
    }
  if (!framebuffersink->use_buffer_pool)
    gst_memory_unmap (framebuffersink->screens[framebuffersink->current_framebuffer_index], &mapinfo);
  return;
}

static void
gst_framebuffersink_put_overlay_image_memcpy(GstFramebufferSink *framebuffersink, GstMemory *vmem,
    uint8_t *src)
{
  GstFramebufferSinkClass *klass = GST_FRAMEBUFFERSINK_GET_CLASS (framebuffersink);
  uint8_t *framebuffer_address;
  GstMapInfo mapinfo;
  gst_memory_map (vmem, &mapinfo, GST_MAP_WRITE);
  framebuffer_address = mapinfo.data;
  if (framebuffersink->overlay_alignment_is_native)
    memcpy(framebuffer_address, src, framebuffersink->info.size);
  else {
    int i;
    int n = GST_VIDEO_INFO_N_PLANES (&framebuffersink->info);
    guintptr offset;
    for (i = 0; i < n; i++) {
      offset = framebuffersink->overlay_plane_offset[i];
      if (GST_VIDEO_INFO_PLANE_STRIDE (&framebuffersink->info, i) ==
          framebuffersink->overlay_scanline_stride[i])
        memcpy(framebuffer_address + offset, src, framebuffersink->overlay_scanline_stride[i]
            * framebuffersink->videosink.height);
      else {
        int y;
        for (y = 0; y < framebuffersink->videosink.height; y++) {
          memcpy(framebuffer_address + offset, src, framebuffersink->source_video_width_in_bytes[i]);
          offset += framebuffersink->overlay_scanline_stride[i];
        }
      }
    }
  }
  gst_memory_unmap (vmem, &mapinfo);
  klass->show_overlay (framebuffersink, framebuffer_address - framebuffersink->framebuffer);
}

static void
gst_framebuffersink_wait_for_vsync (GstFramebufferSink * framebuffersink)
{
  if (ioctl (framebuffersink->fd, FBIO_WAITFORVSYNC, NULL)) {
    GST_ERROR_OBJECT(framebuffersink, "FBIO_WAITFORVSYNC call failed. Disabling vsync.");
    framebuffersink->vsync = FALSE;
  }
}

static void
gst_framebuffersink_pan_display (GstFramebufferSink * framebuffersink, int xoffset,
int yoffset) {
  int old_xoffset = framebuffersink->varinfo.xoffset;
  int old_yoffset = framebuffersink->varinfo.yoffset;
  framebuffersink->varinfo.xoffset = xoffset;
  framebuffersink->varinfo.yoffset = yoffset;
  if (ioctl (framebuffersink->fd, FBIOPAN_DISPLAY, &framebuffersink->varinfo)) {
    GST_ERROR_OBJECT (framebuffersink, "FBIOPAN_DISPLAY call failed");
    framebuffersink->varinfo.xoffset = old_xoffset;
    framebuffersink->varinfo.yoffset = old_yoffset;
  }
}

static void
gst_framebuffersink_pan_to_framebuffer (GstFramebufferSink * framebuffersink, int buffer) {
  gst_framebuffersink_pan_display(framebuffersink, 0, framebuffersink->varinfo.yres * buffer);
}

static void
gst_framebuffersink_put_image_pan(GstFramebufferSink * framebuffersink, uint8_t *fbdata) {
  int buffer;
  buffer = (fbdata - framebuffersink->framebuffer) / (framebuffersink->varinfo.yres *
      framebuffersink->fixinfo.line_length);
  if (framebuffersink->vsync && !framebuffersink->pan_does_vsync)
    gst_framebuffersink_wait_for_vsync(framebuffersink);
  gst_framebuffersink_pan_to_framebuffer(framebuffersink, buffer);
}

/* Start function, called when resources should be allocated. */

static gboolean
gst_framebuffersink_start (GstBaseSink *sink)
{
  GstFramebufferSink *framebuffersink = GST_FRAMEBUFFERSINK (sink);
  GstFramebufferSinkClass *klass = GST_FRAMEBUFFERSINK_GET_CLASS (framebuffersink);

  GST_DEBUG_OBJECT (framebuffersink, "start");
  g_return_if_fail (GST_IS_FRAMEBUFFERSINK (framebuffersink));

  if (!gst_framebuffersink_open_device(framebuffersink))
    return FALSE;

  framebuffersink->open_hardware_success = FALSE;
  if (framebuffersink->use_hardware_overlay) {
    framebuffersink->open_hardware_success = klass->open_hardware (framebuffersink);
    if (!framebuffersink->open_hardware_success)
      /* Disable any hardware acceleration features. */
      framebuffersink->use_hardware_overlay = FALSE;
  }

  if (framebuffersink->full_screen) {
      framebuffersink->requested_video_width = framebuffersink->varinfo.xres;
      framebuffersink->requested_video_height = framebuffersink->varinfo.yres;
  }

  framebuffersink->current_framebuffer_index = 0;

  framebuffersink->screens = NULL;

  /* Reset overlay types. */
  framebuffersink->overlay_formats_supported = gst_framebuffersink_get_supported_overlay_formats (framebuffersink);
  /* Set overlay types if supported. */
  if (framebuffersink->use_hardware_overlay) {
    framebuffersink->current_overlay_index = 0;
    framebuffersink->overlay_formats_supported = klass->get_supported_overlay_formats (framebuffersink);
  }

  framebuffersink->stats_video_frames_video_memory = 0;
  framebuffersink->stats_video_frames_system_memory = 0;
  framebuffersink->stats_overlay_frames_video_memory = 0;
  framebuffersink->stats_overlay_frames_system_memory = 0;

  return TRUE;
}

/* Sets size and frame-rate preferences on caps. */

static void
gst_framebuffersink_caps_set_preferences (GstFramebufferSink *framebuffersink, GstCaps *caps,
    gboolean no_par)
{
  /* If hardware scaling is supported, and a specific video size is requested, allow any reasonable size */
  /* (except when the width/height_before_scaler properties are set) and use the scaler. */
  if ((framebuffersink->requested_video_width != 0 || framebuffersink->requested_video_height != 0)
      && gst_framebuffersink_video_format_supported_by_overlay (framebuffersink, GST_VIDEO_FORMAT_BGRx)) {
    if (framebuffersink->width_before_scaling != 0)
      gst_caps_set_simple (caps, "width", G_TYPE_INT, framebuffersink->width_before_scaling, NULL);
    else
       gst_caps_set_simple (caps, "width", GST_TYPE_INT_RANGE, 1, framebuffersink->varinfo.xres, NULL);
    if (framebuffersink->height_before_scaling != 0)
      gst_caps_set_simple (caps, "height", G_TYPE_INT, framebuffersink->height_before_scaling, NULL);
    else
      gst_caps_set_simple (caps, "height", GST_TYPE_INT_RANGE, 1, framebuffersink->varinfo.yres, NULL);
    goto skip_video_size_request;
  }

  /* Honour video size requests if the preserve_par property is not set; */
  /* otherwise set the allowable range up to the screen size. */
  if ((!framebuffersink->preserve_par || no_par) && framebuffersink->requested_video_width != 0)
    gst_caps_set_simple(caps,
        "width", G_TYPE_INT, framebuffersink->requested_video_width, NULL);
  else
    gst_caps_set_simple (caps, "width", GST_TYPE_INT_RANGE, 1, framebuffersink->varinfo.xres, NULL);
  if ((!framebuffersink->preserve_par || no_par) && framebuffersink->requested_video_height != 0)
    gst_caps_set_simple(caps,
        "height", G_TYPE_INT, framebuffersink->requested_video_height, NULL);
  else
    gst_caps_set_simple (caps, "height", GST_TYPE_INT_RANGE, 1, framebuffersink->varinfo.yres, NULL);

skip_video_size_request:

  /* Honour frames per second requests. */
  if (framebuffersink->fps != 0)
    gst_caps_set_simple(caps,
        "framerate", GST_TYPE_FRACTION, framebuffersink->fps, 1, NULL);
  else
    gst_caps_set_simple(caps, "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
}

/* Return default caps, or NULL if no default caps could be not generated. */

static GstCaps *gst_framebuffersink_get_default_caps (GstFramebufferSink *framebuffersink) {
  GstCaps *caps;
  int depth;
  GstCaps *framebuffer_caps;
  GstVideoFormat *f;

  if (framebuffersink->framebuffer_format == GST_VIDEO_FORMAT_UNKNOWN)
    goto unknown_format;

  caps = gst_caps_new_empty();

  /* First add any specific overlay formats that are supported. */
  /* They will have precedence over the standard framebuffer format. */

  f = framebuffersink->overlay_formats_supported;
  while (*f != GST_VIDEO_FORMAT_UNKNOWN) {
    if (*f != framebuffersink->framebuffer_format) {
      GstCaps *overlay_caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING,
          gst_video_format_to_string(*f), NULL);
      gst_caps_append(caps, overlay_caps);
    }
    f++;
  }

  framebuffer_caps = gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING,
    gst_video_format_to_string (framebuffersink->framebuffer_format), NULL);
  gst_caps_append(caps, framebuffer_caps);

  return caps;

unknown_format:

  depth = framebuffersink->varinfo.red.length + framebuffersink->varinfo.green.length
      + framebuffersink->varinfo.blue.length;

  GST_WARNING_OBJECT (framebuffersink, "could not map fbdev format to GstVideoFormat: "
      "depth=%u, bpp=%u, endianness=%u, rmask=0x%08x, gmask=0x%08x, "
      "bmask=0x%08x, tmask=0x%08x", depth, framebuffersink->varinfo.bits_per_pixel,
      framebuffersink->endianness, framebuffersink->rmask, framebuffersink->gmask,
      framebuffersink->bmask, 0);
  return NULL;
}

/* Helper function to parse caps in a fool-proof manner and pick our preferred video format */
/* from caps. */

static GstVideoFormat gst_framebuffersink_get_preferred_video_format_from_caps (
    GstFramebufferSink *framebuffersink, GstCaps *caps)
{
  GstCaps *ncaps;
  int n;
  int i;
  GstVideoFormat best_format = GST_VIDEO_FORMAT_UNKNOWN;
  int best_rank = G_MAXINT;
  GstVideoFormat preferred_overlay_format_from_property = GST_VIDEO_FORMAT_UNKNOWN;
  if (framebuffersink->preferred_overlay_format_str != NULL) {
    preferred_overlay_format_from_property = gst_video_format_from_string (
        framebuffersink->preferred_overlay_format_str);
    if (preferred_overlay_format_from_property == GST_VIDEO_FORMAT_UNKNOWN)
      GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink,
          "Unknown video format in overlay-format property");
  }
  ncaps = gst_caps_copy (caps);
  ncaps = gst_caps_normalize (ncaps);
  n = gst_caps_get_size (ncaps);
  for (i = 0; i < n; i++) {
    GstStructure *str = gst_caps_get_structure (ncaps, i);
    const char *format_s;
    format_s = gst_structure_get_string (str, "format");
    if (format_s != NULL) {
      GstVideoFormat f = gst_video_format_from_string (format_s);
      int r;
      if (!gst_framebuffersink_video_format_supported_by_overlay (framebuffersink, f)) {
         /* Regular formats that are not supported by the overlay get a rank that is based */
         /* on the order in the caps but always behind the overlay formats. */
         if (i + 1000000 < best_rank) {
           best_format = f;
           best_rank = i + 1000000;
         }
         continue;
      }
      if (preferred_overlay_format_from_property != GST_VIDEO_FORMAT_UNKNOWN
      && f == preferred_overlay_format_from_property)
        r = - 1;
      else
        r = gst_framebuffersink_get_overlay_format_rank (framebuffersink, f);
      if (r < best_rank) {
        best_format = f;
        best_rank = r;
      }
    }
  }
  gst_caps_unref (ncaps);
  return best_format;
}

/* Get caps returns caps modified with specific user settings. */

static GstCaps *
gst_framebuffersink_get_caps (GstBaseSink * sink, GstCaps * filter)
{
  GstFramebufferSink *framebuffersink = GST_FRAMEBUFFERSINK (sink);
  GstCaps *caps;
  int i;
  int w, h;
  int par_n, par_d;
  int n;
  gboolean no_par;
  const char *format_str = NULL;
  char s[80];

  GST_DEBUG_OBJECT (framebuffersink, "get_caps");
  g_return_if_fail (GST_IS_FRAMEBUFFERSINK (framebuffersink));

#ifdef EXTRA_DEBUG
  if (!framebuffersink->silent)
    g_print ("get_caps: filter caps: %" GST_PTR_FORMAT "\n", filter);
#endif

  /* If the framebuffer device hasn't been initialized yet, return the template caps. */
  if (!framebuffersink->framebuffer) {
    caps = gst_caps_ref (gst_static_pad_template_get_caps (&gst_framebuffersink_sink_template));
    return caps;
  }

  /* Return the current stored caps when filter is NULL and */
  /* we have stored caps. */
  if (filter == NULL && framebuffersink->have_caps) {
    caps = gst_caps_ref (framebuffersink->caps);
    goto done_no_store;
  }

  if (framebuffersink->adjusted_dimensions) {
    GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, "get_caps called after dimensions adjusted");
//    g_print ("get_caps: filter caps: %" GST_PTR_FORMAT "\n", filter);
//    g_print ("get_caps: stored caps: %" GST_PTR_FORMAT "\n", framebuffersink->caps);
    caps = gst_caps_ref (framebuffersink->caps);
    goto done_no_store;
  }

  /* Check whether upstream is reporting video dimensions and par. */
  no_par = TRUE;
  if (filter == NULL)
    n = 0;
  else
    n = gst_caps_get_size (filter);
  w = 0;
  h = 0;
  par_n = 0;
  par_d = 0;
  for (i = 0; i < n; i++) {
    const gchar *fs;
    GstStructure *str = gst_caps_get_structure (filter, i);
    gst_structure_get_int (str, "width", &w);
    gst_structure_get_int (str, "height", &h);
    if (gst_structure_has_field (str, "pixel-aspect-ratio")) {
      no_par = FALSE;
      gst_structure_get_fraction (str, "pixel-aspect-ratio", &par_n, &par_d);
    }
    fs = gst_structure_get_string (str, "format");
    if (fs != NULL && format_str == NULL)
      format_str = fs;
  }

  /* Set the caps to the stored ones if we have them, otherwise generate default caps. */
  if (framebuffersink->have_caps)
    caps = gst_caps_ref (framebuffersink->caps);
  else {
    caps = gst_framebuffersink_get_default_caps(framebuffersink);
    if (caps == NULL)
      return NULL;
    if (filter == NULL)
       no_par = FALSE;
    gst_framebuffersink_caps_set_preferences(framebuffersink, caps, no_par);
  }

  if (filter == NULL)
    goto done_no_intersect;

  /* Wait until upstream reports the video dimensions. */
  if (w == 0 || h == 0)
    /* Upstream has not yet confirmed a video size */
    goto done;

  /* Upstream has confirmed a video size */

  /* Reconfigure output size and preserve aspect ratio when preserve-par */
  /* property is set and upstream has reported an aspect ratio. */
  if (framebuffersink->preserve_par && par_d != 0 && par_n != 0) {
      double ratio;
      ratio = (double) w / h;
      if (framebuffersink->requested_video_width != 0 ||
      framebuffersink->requested_video_height != 0) {
        int output_width;
        int output_height;
        double r;
        gboolean adjusted_aspect = FALSE;
        if (framebuffersink->requested_video_width != 0) {
          output_width = framebuffersink->requested_video_width;
          if (framebuffersink->requested_video_height != 0)
            /* Both requested width and height specified. */
            output_height = framebuffersink->requested_video_height;
          else {
            output_height = (double) output_width / ratio;
            adjusted_aspect = TRUE;
          }
        }
        else if (framebuffersink->requested_video_height != 0) {
          output_height = framebuffersink->requested_video_height;
          output_width = (double) output_height * ratio;
          adjusted_aspect = TRUE;
        }

        r = (double) output_width / output_height;
        if (r > ratio + 0.01) {
          /* Insert black borders on the sides. */
          output_width = output_width * ratio / r;
          adjusted_aspect = TRUE;
        }
        else if (r < ratio - 0.01) {
          /* Insert black borders on the top and bottom. */
          output_height = output_height * r / ratio;
          adjusted_aspect = TRUE;
        }

        if (output_width != w || output_height != h) {
          GstCaps *icaps;
          GstVideoFormat format;

          /* Intersect and set new output dimensions. */
          icaps = gst_caps_intersect (caps, filter);
          gst_caps_unref (caps);
          caps = icaps;

          /* Try to get the video format from caps that is our preferred video */
          /* format (supported by overlay). */
          format = gst_framebuffersink_get_preferred_video_format_from_caps(
            framebuffersink, caps);
          if (gst_framebuffersink_video_format_supported_by_overlay (framebuffersink, format)) {
            /* Set the preferred format. */
            gst_caps_set_simple(caps,
                "format", G_TYPE_STRING, gst_video_format_to_string (format), NULL);
            caps = gst_caps_simplify(caps);
          }
          /* If we are not using the hardware overlay, inform upstream to */
          /* scale to the new size. */
          else if (!gst_framebuffersink_video_format_supported_by_overlay (framebuffersink,
              format)) {
            gst_caps_set_simple(caps,
                "width", G_TYPE_INT, output_width, NULL);
            gst_caps_set_simple(caps,
                "height", G_TYPE_INT, output_height, NULL);
            gst_caps_set_simple(caps, "pixel-aspect-ratio", GST_TYPE_FRACTION,
                par_n * output_width * h, par_d * output_height * w, NULL);
          }
          if (adjusted_aspect) {
            sprintf(s, "Preserve aspect ratio: Adjusted output dimensions to %d x %d",
                output_width, output_height);
            GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, s);
          }

          framebuffersink->adjusted_dimensions = TRUE;
          framebuffersink->adjusted_width = output_width;
          framebuffersink->adjusted_height = output_height;

          goto done_no_intersect;
        }
      }
  }
  else {
    // Do not preserve aspect ratio.
    if (framebuffersink->requested_video_width != 0 ||
    framebuffersink->requested_video_height != 0) {
      int output_width = w;
      int output_height = h;
      if (framebuffersink->requested_video_width != 0)
        output_width = framebuffersink->requested_video_width;
      if (framebuffersink->requested_video_height != 0)
        output_height = framebuffersink->requested_video_height;

      if (output_width != w || output_height != h) {
          GstCaps *icaps;
          GstVideoFormat format;

          /* Intersect and set new output dimensions. */
          icaps = gst_caps_intersect (caps, filter);
          gst_caps_unref (caps);
          caps = icaps;

          /* Try to get the video format from caps that is our preferred video */
          /* format (supported by overlay). */
          format = gst_framebuffersink_get_preferred_video_format_from_caps(
            framebuffersink, caps);
          if (gst_framebuffersink_video_format_supported_by_overlay (framebuffersink, format)) {
            /* Set the preferred format. */
            gst_caps_set_simple(caps,
                "format", G_TYPE_STRING, gst_video_format_to_string (format), NULL);
            caps = gst_caps_simplify(caps);
          }

          framebuffersink->adjusted_dimensions = TRUE;
          framebuffersink->adjusted_width = output_width;
          framebuffersink->adjusted_height = output_height;

          goto done_no_intersect;
      }

      framebuffersink->adjusted_dimensions = TRUE;
      framebuffersink->adjusted_width = output_width;
      framebuffersink->adjusted_height = output_height;
    }
  }

done:

  /* Return the intersection of the current caps with the filter caps. */
  if (filter != NULL) {
    GstCaps *icaps;

    icaps = gst_caps_intersect (caps, filter);
    gst_caps_unref (caps);
    caps = icaps;
  }

done_no_intersect:

  /* Store the updated caps. */
  if (framebuffersink->have_caps)
    gst_caps_unref (framebuffersink->caps);
  framebuffersink->have_caps = TRUE;
  framebuffersink->caps = gst_caps_ref (caps);

done_no_store:

#ifdef EXTRA_DEBUG
  if (!framebuffersink->silent)
    g_print ("get_caps: returned caps: %" GST_PTR_FORMAT "\n", caps);
#endif

  return caps;

}

/* Debugging function. */

static void gst_buffer_print (GstFramebufferSink *framebuffersink, GstBuffer *buf)
{
  GstMemory *memory;
  GstMapInfo mapinfo;
  g_print("Number of memory areas in buffer: %d, total size: %d\n",
     gst_buffer_n_memory (buf), gst_buffer_get_size(buf));
  memory = gst_buffer_peek_memory(buf, 0);
  if (!memory) {
    g_print("No memory in buffer.\n");
  }
  else {
    gst_memory_map(memory, &mapinfo, GST_MAP_WRITE);
    g_print("Framebuffer in buffer pool = %08X\n", (guintptr)mapinfo.data);
    gst_memory_unmap(memory, &mapinfo);
  }
}

static void
gst_framebuffersink_allocation_params_init (GstFramebufferSink *framebuffersink,
GstAllocationParams *allocation_params)
{
  int i;
  gst_allocation_params_init(allocation_params);
  allocation_params->flags = 0;
  allocation_params->prefix = 0;
  allocation_params->padding = 0;
  if (framebuffersink->use_hardware_overlay)
    allocation_params->align = framebuffersink->overlay_alignment;
  else {
    /* Determine the minimum alignment of the framebuffer screen pages. */
    /* The minimum guaranteed alignment is word-aligned (align = 3). */
    for (i = 8; i <= 4096; i <<= 1)
      if (framebuffersink->fixinfo.line_length & (i - 1))
        break;
    allocation_params->align = (i >> 1) - 1;
  }
}

/* This function is called from set_caps when we are configured with */
/* use_buffer_pool=true, and from propose_allocation */

static GstBufferPool *
gst_framebuffersink_allocate_buffer_pool (GstFramebufferSink *framebuffersink, GstCaps *caps,
GstVideoInfo *info) {
  GstStructure *config;
  GstBufferPool *newpool, *oldpool;
  GstAllocator *allocator;
  GstAllocationParams allocation_params;
  int offset;
  int n;
  char s[256];

  GST_DEBUG("allocate_buffer_pool, caps: %" GST_PTR_FORMAT, caps);

  offset = 0;
  /* When using hardware overlay, buffers are allocated starting after the first visible screen. */
  if (framebuffersink->use_hardware_overlay)
    offset = framebuffersink->fixinfo.line_length * framebuffersink->varinfo.yres;

  if (framebuffersink->use_hardware_overlay && framebuffersink->screens == NULL) {
    framebuffersink->screens = malloc (sizeof (GstMemory *) * 1);
    framebuffersink->screens[0] = gst_framebuffersink_video_memory_alloc (offset, 0);
  }

  /* Create a new pool for the new configuration. */
  newpool = gst_video_buffer_pool_new ();

  config = gst_buffer_pool_get_config (newpool);

  n = framebuffersink->nu_framebuffers_used;
  if (framebuffersink->use_hardware_overlay)
    n = framebuffersink->nu_overlays_used;

#ifdef HALF_POOLS
  gst_buffer_pool_config_set_params (config, caps, info->size, n / 2, n / 2);
#else
  gst_buffer_pool_config_set_params (config, caps, info->size, n, n);
#endif

  allocator = gst_allocator_find ("framebuffersink_video_memory");

  gst_framebuffersink_allocation_params_init (framebuffersink, &allocation_params);

  gst_buffer_pool_config_set_allocator (config, allocator, &allocation_params);
//  gst_buffer_pool_config_set_allocator (config, NULL,
//      &framebuffersink->allocation_params);
  if (!gst_buffer_pool_set_config (newpool, config))
    goto config_failed;

//  GST_OBJECT_LOCK (framebuffersink);
//  oldpool = framebuffersink->pool;
//  framebuffersink->pool = newpool;
//  GST_OBJECT_UNLOCK (framebuffersink);

//  if (oldpool) {
//    gst_object_unref (oldpool);
//  }

  g_sprintf(s, "Succesfully allocated buffer pool (frame size %d, %d buffers, alignment to %d byte boundary)",
    info->size, n, allocation_params.align + 1);
  GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink, s);

#if 0
  if (!gst_buffer_pool_set_active(framebuffersink->pool, TRUE))
   goto activation_failed;

  GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink, "Succesfully activated buffer pool");
#endif

  return newpool;

/* ERRORS */
config_failed:
  {
    GST_ERROR_OBJECT (framebuffersink, "Failed to set buffer pool config");
    return NULL;
  }
activation_failed:
  {
    GST_ERROR_OBJECT(framebuffersink, "Activation of buffer pool failed");
    return NULL;
  }
}


static void
gst_framebuffersink_calculate_plane_widths(GstFramebufferSink *framebuffersink, GstVideoInfo *info)
{
  /* Iterate components instead of planes. The width for planes which contain multiple components */
  /* will be written multiple times but should be the same. */
  int n = GST_VIDEO_INFO_N_COMPONENTS (info);
  int i;
  for (i = 0; i < n; i++) {
    int plane = GST_VIDEO_INFO_COMP_PLANE(info, i);
    framebuffersink->source_video_width_in_bytes[plane] = GST_VIDEO_FORMAT_INFO_SCALE_WIDTH (
        info->finfo, i, GST_VIDEO_INFO_WIDTH(info)) * GST_VIDEO_INFO_COMP_PSTRIDE(info, i);
//    g_print("component %d, plane %d, pixel stride %d\n", i, plane,
//        GST_VIDEO_INFO_COMP_PSTRIDE(info, i));
  }
}

static gboolean
gst_framebuffersink_check_overlay_alignment(GstFramebufferSink *framebuffersink, GstVideoInfo *info,
gboolean set_overlay_alignment)
{
  /* Make sure the source video data obeys the alignment restrictions imposed by the driver. */
  int n = GST_VIDEO_INFO_N_PLANES (info);
  int i;
  if (set_overlay_alignment)
    framebuffersink->overlay_alignment_is_native = FALSE;
  for (i = 0; i < n; i++) {
    int offset;
    int stride = GST_VIDEO_INFO_PLANE_STRIDE(info, i);
    int stride_aligned;
    int video_data_width;
    if ((stride & framebuffersink->overlay_scanline_alignment) != 0) {
      GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink,
          "Video scanline alignment does not meet hardware overlay restrictions (stride doesn't match)");
      return FALSE;
    }
    stride_aligned = ALIGNMENT_GET_ALIGNED(stride, framebuffersink->overlay_scanline_alignment);
    if (framebuffersink->overlay_scanline_alignment_is_fixed &&
        stride_aligned != ALIGNMENT_GET_ALIGNED(framebuffersink->source_video_width_in_bytes[i],
        framebuffersink->overlay_scanline_alignment)) {
      GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink,
          "Video scanline alignment does not meet hardware overlay restrictions (video alignment "
          "too wide)");
      return FALSE;
    }
    offset = GST_VIDEO_INFO_PLANE_OFFSET(info, i);
    if ((offset & framebuffersink->overlay_plane_alignment) != 0) {
      GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink,
          "Video plane alignment does not meet hardware overlay restrictions");
      return FALSE;
    }
    if (set_overlay_alignment) {
      framebuffersink->overlay_scanline_stride[i] = stride;
      framebuffersink->overlay_plane_offset[i] = offset;
    }
  }
  if (set_overlay_alignment) {
    framebuffersink->overlay_size = info->size;
    framebuffersink->overlay_alignment_is_native = TRUE;
  }
  return TRUE;
}

/* Calculate the organization of overlay data in video memory. */

static void gst_framebuffersink_calculate_overlay_size(GstFramebufferSink *framebuffersink,
GstVideoInfo *info) {
  int i;
  int n = GST_VIDEO_INFO_N_PLANES (info);
  int offset = 0;
  for (i = 0; i < n; i++) {
    int j;
    for (j = 0; j < GST_VIDEO_INFO_N_COMPONENTS (info); j++)
      if (GST_VIDEO_INFO_COMP_PLANE (info, j) == i)
        break;
    offset += ALIGNMENT_GET_ALIGN_BYTES(offset, framebuffersink->overlay_plane_alignment);
    framebuffersink->overlay_plane_offset[i] = offset;
    int stride = ALIGNMENT_GET_ALIGNED(framebuffersink->source_video_width_in_bytes[i],
        framebuffersink->overlay_scanline_alignment);
    framebuffersink->overlay_scanline_stride[i] = stride;
    offset += GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT (info->finfo, j, GST_VIDEO_INFO_HEIGHT (info)) * stride;
  }
  framebuffersink->overlay_size = offset;
}

/* This function is called when the GstBaseSink should prepare itself */
/* for a given media format. It practice it may be called twice with the */
/* same caps, so we have to detect that. */

static gboolean
gst_framebuffersink_set_caps (GstBaseSink * sink, GstCaps * caps)
{
  GstFramebufferSink *framebuffersink = GST_FRAMEBUFFERSINK (sink);
  GstFramebufferSinkClass *klass = GST_FRAMEBUFFERSINK_GET_CLASS (framebuffersink);
  GstVideoInfo info;
  GstVideoFormat matched_overlay_format;
  int i;

  if (!gst_video_info_from_caps (&info, caps))
    goto invalid_format;

  if (gst_video_info_is_equal(&info, &framebuffersink->info)) {
    GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, "set_caps called with same caps");
    return TRUE;
   }

  if (!framebuffersink->silent)
    g_print ("Negotiated caps: %" GST_PTR_FORMAT "\n", caps);

  /* Set the video parameters. */
  framebuffersink->fps_n = info.fps_n;
  framebuffersink->fps_d = info.fps_d;

  framebuffersink->videosink.width = info.width;
  framebuffersink->videosink.height = info.height;

  framebuffersink->framebuffer_video_width_in_bytes = framebuffersink->videosink.width *
      framebuffersink->bytespp;

  gst_framebuffersink_calculate_plane_widths(framebuffersink, &info);

  if (framebuffersink->framebuffer_video_width_in_bytes > framebuffersink->fixinfo.line_length)
    framebuffersink->framebuffer_video_width_in_bytes = framebuffersink->fixinfo.line_length;

  framebuffersink->lines = framebuffersink->videosink.height;
  if (framebuffersink->lines > framebuffersink->varinfo.yres)
    framebuffersink->lines = framebuffersink->varinfo.yres;

  if (framebuffersink->videosink.width <= 0 || framebuffersink->videosink.height <= 0)
    goto no_display_size;

  if (framebuffersink->flip_buffers > 0) {
    if (framebuffersink->flip_buffers < framebuffersink->max_framebuffers)
      framebuffersink->max_framebuffers = framebuffersink->flip_buffers;
  }

  /* Make sure all framebuffers can be panned to. */
  if (framebuffersink->varinfo.yres_virtual < framebuffersink->max_framebuffers * framebuffersink->varinfo.yres)
    if (!gst_framebuffersink_set_device_virtual_size(framebuffersink, framebuffersink->varinfo.xres_virtual,
    framebuffersink->max_framebuffers * framebuffersink->varinfo.yres)) {
      GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink,
          "Could not set the device virtual screen size large enough to support all buffers");
      framebuffersink->max_framebuffers = framebuffersink->varinfo.yres_virtual / framebuffersink->varinfo.yres;
    }

  matched_overlay_format = GST_VIDEO_INFO_FORMAT (&info);
  if (!gst_framebuffersink_video_format_supported_by_overlay (framebuffersink, matched_overlay_format))
    matched_overlay_format = GST_VIDEO_FORMAT_UNKNOWN;

  if (framebuffersink->adjusted_dimensions) {
    framebuffersink->scaled_width = framebuffersink->adjusted_width;
    framebuffersink->scaled_height = framebuffersink->adjusted_height;
  }
  else {
    framebuffersink->scaled_width = info.width;
    framebuffersink->scaled_height = info.height;
    /* When using the hardware scaler, and upstream didn't call get_caps with */
    /* the negotiated caps, update the output dimensions for the scaler. */
    if (matched_overlay_format != GST_VIDEO_FORMAT_UNKNOWN) {
      if (framebuffersink->requested_video_width != 0 &&
          framebuffersink->requested_video_width != info.width)
        framebuffersink->scaled_width = framebuffersink->requested_video_width;
      if (framebuffersink->requested_video_height != 0 &&
          framebuffersink->requested_video_height != info.height)
        framebuffersink->scaled_height = framebuffersink->requested_video_height;
    }
  }

  /* If the video size is smaller than the screen, center the video. */
  framebuffersink->cx = ((int) framebuffersink->varinfo.xres - framebuffersink->scaled_width) / 2;
  if (framebuffersink->cx < 0)
    framebuffersink->cx = 0;

  framebuffersink->cy = ((int) framebuffersink->varinfo.yres - framebuffersink->scaled_height) / 2;
  if (framebuffersink->cy < 0)
    framebuffersink->cy = 0;

  /* Check whether we will use the hardware overlay feature. */
  if (((framebuffersink->scaled_width != framebuffersink->videosink.width
      || framebuffersink->scaled_height != framebuffersink->videosink.height)
      || matched_overlay_format != framebuffersink->framebuffer_format)
      && matched_overlay_format != GST_VIDEO_FORMAT_UNKNOWN
      && framebuffersink->use_hardware_overlay) {
    int max_overlays;
    int first_overlay_offset;
    /* The video dimensions are different from the requested ones, or the video format is not equal */
    /* to the framebuffer format, and we are allowed to use the hardware overlay. */
    klass->get_alignment_restrictions(framebuffersink, matched_overlay_format,
        &framebuffersink->overlay_alignment, &framebuffersink->overlay_scanline_alignment,
        &framebuffersink->overlay_plane_alignment, &framebuffersink->overlay_scanline_alignment_is_fixed);
    /* If the supplied format matches the hardware restriction, use that format, otherwise */
    /* define a different format for overlays in video memory. */
    if (!gst_framebuffersink_check_overlay_alignment(framebuffersink, &info, TRUE))
      gst_framebuffersink_calculate_overlay_size(framebuffersink, &info);
    /* Calculate how may overlays fit in the available video memory (after the visible */
    /* screen. */
    first_overlay_offset = framebuffersink->fixinfo.line_length * framebuffersink->varinfo.yres;
    ALIGNMENT_APPLY(first_overlay_offset, framebuffersink->overlay_alignment);
    max_overlays = (framebuffersink->framebuffer_map_size - first_overlay_offset)
        / ALIGNMENT_GET_ALIGNED(framebuffersink->overlay_size, framebuffersink->overlay_alignment);
    /* Limit the number of overlays used, unless the agressive max video memory setting is enabled. */
    if (framebuffersink->max_video_memory_property != - 2 && max_overlays > 30)
      max_overlays = 30;
    if (gst_framebuffersink_video_format_supported_by_overlay (framebuffersink, matched_overlay_format) &&
        max_overlays >= 2 && klass->prepare_overlay (framebuffersink, matched_overlay_format)) {
      /* Use the hardware overlay. */
      framebuffersink->nu_framebuffers_used = framebuffersink->max_framebuffers;
      framebuffersink->nu_overlays_used = max_overlays;
      if (framebuffersink->use_buffer_pool) {
        if (framebuffersink->overlay_alignment_is_native) {
          GstBufferPool *pool = gst_framebuffersink_allocate_buffer_pool (framebuffersink, caps, &info);
          if (pool) {
            /* Use buffer pool. */
            framebuffersink->pool = pool;
            if (!framebuffersink->silent)
              GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink,
                "Using custom buffer pool (streaming directly to video memory)");
            goto success_overlay;
          }
        }
        framebuffersink->use_buffer_pool = FALSE;
        if (!framebuffersink->silent) {
          if (!framebuffersink->overlay_alignment_is_native)
            GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink,
                "Alignment restrictions make overlay buffer-pool mode impossible for this video size");
          GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, "Falling back to non buffer-pool mode");
        }
      }
      /* Not using buffer pool. Using a lot of off-screen buffers may not help. */
      if (framebuffersink->nu_overlays_used > 8)
        framebuffersink->nu_overlays_used = 8;
      goto success_overlay;
    }
  }

  if (framebuffersink->use_hardware_overlay) {
    GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, "Disabling hardware overlay");
    framebuffersink->use_hardware_overlay = FALSE;
  }

  if (matched_overlay_format != GST_VIDEO_FORMAT_UNKNOWN &&
      matched_overlay_format != framebuffersink->framebuffer_format)
    goto overlay_failed;

reconfigure:

  /* When using buffer pools, do the appropriate checks and allocate a */
  /* new buffer pool. */
  if (framebuffersink->use_buffer_pool) {
    if (framebuffersink->framebuffer_video_width_in_bytes != framebuffersink->fixinfo.line_length) {
      GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, "Cannot use buffer pool in video "
          "memory because video width is not equal to the configured framebuffer "
          "width");
      framebuffersink->use_buffer_pool = FALSE;
    }
    if (framebuffersink->max_framebuffers < 2) {
      GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink,
          "Not enough framebuffer memory to use a buffer pool (need at least two framebuffers)");
      framebuffersink->use_buffer_pool = FALSE;
    }
  }
  if (framebuffersink->max_framebuffers >= 2) {
    framebuffersink->nu_framebuffers_used = framebuffersink->max_framebuffers;
    /* Using a fair number of buffers could be advantageous, but use no more than 10. */
    /* by default except if the agressive video memory property seting is enabled. */
    if (framebuffersink->use_buffer_pool) {
      if (framebuffersink->flip_buffers == 0 && framebuffersink->nu_framebuffers_used > 10
      && framebuffersink->max_video_memory_property != - 2)
        framebuffersink->nu_framebuffers_used = 10;
    }
    else
      /* When not using a buffer pool, only a few buffers are required for page flipping. */
      if (framebuffersink->flip_buffers == 0 && framebuffersink->nu_framebuffers_used > 3)
        framebuffersink->nu_framebuffers_used = 3;
    if (!framebuffersink->silent) {
      char s[80];
      g_sprintf (s, "Using %d framebuffers for page flipping.\n",
          framebuffersink->nu_framebuffers_used);
      GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, s);
    }
  }
  if (framebuffersink->use_buffer_pool) {
    GstBufferPool *pool;
    pool = gst_framebuffersink_allocate_buffer_pool (framebuffersink, caps, &info);
    if (pool) {
      framebuffersink->pool = pool;
      if (!framebuffersink->silent)
         GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink,
             "Using custom buffer pool (streaming directly to video memory)");
      goto success;
    }
    framebuffersink->use_buffer_pool = FALSE;
    if (!framebuffersink->silent)
      GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, "Falling back to non buffer-pool mode");
    goto reconfigure;
  }

success:

  if (!framebuffersink->use_buffer_pool) {
    framebuffersink->screens = malloc (sizeof (GstMemory *) * framebuffersink->nu_framebuffers_used);
    for (i = 0; i < framebuffersink->nu_framebuffers_used; i++) {
      framebuffersink->screens[i] = gst_framebuffersink_video_memory_alloc(
        framebuffersink->varinfo.yres * framebuffersink->fixinfo.line_length, 0);
      if (framebuffersink->screens[i] == NULL) {
        framebuffersink->nu_framebuffers_used = i;
        break;
      }
    }
  }

finish:

  framebuffersink->info = info;

  /* Clear all used framebuffers to black. */
  if (framebuffersink->clear) {
    if (framebuffersink->use_hardware_overlay)
      gst_framebuffersink_clear_screen (framebuffersink, 0);
    else
      if (!framebuffersink->use_buffer_pool)
        for (i = 0; i < framebuffersink->nu_framebuffers_used; i++)
          gst_framebuffersink_clear_screen (framebuffersink, i);
  }
  return TRUE;

success_overlay:

  if (!framebuffersink->use_buffer_pool) {
    framebuffersink->screens = malloc (sizeof (GstMemory *));
    framebuffersink->screens[0] = gst_framebuffersink_video_memory_alloc(
        framebuffersink->varinfo.yres * framebuffersink->fixinfo.line_length, 0);
    framebuffersink->overlays = malloc (sizeof (GstMemory *) * framebuffersink->nu_overlays_used);
    for (i = 0; i < framebuffersink->nu_overlays_used; i++) {
      framebuffersink->overlays[i] = gst_framebuffersink_video_memory_alloc (info.size,
          framebuffersink->overlay_alignment);
      if (framebuffersink->overlays[i] == NULL) {
        framebuffersink->nu_overlays_used = i;
        break;
      }
    }
  }

  if (!framebuffersink->silent) {
    char s[128];
    sprintf(s, "Using one framebuffer plus %d overlays in video memory",
        framebuffersink->nu_overlays_used);
    GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, s);
  }
  goto finish;

/* ERRORS */
invalid_format:
  {
    GST_ERROR_OBJECT (framebuffersink,
        "Could not locate image format from caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
no_display_size:
  {
    GST_ERROR_OBJECT (framebuffersink,
        "No video size configured, caps: %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
overlay_failed:
  {
    GST_ERROR_OBJECT (framebuffersink,
        "Cannot not handle overlay format (hardware overlay failed)");
    return FALSE;
  }
}

/* The stop function should release resources. */

static gboolean
gst_framebuffersink_stop (GstBaseSink * sink)
{
  GstFramebufferSink *framebuffersink = GST_FRAMEBUFFERSINK (sink);
  GstFramebufferSinkClass *klass = GST_FRAMEBUFFERSINK_GET_CLASS (framebuffersink);
  char s[128];

  GST_DEBUG_OBJECT (framebuffersink, "stop");
  g_return_if_fail (GST_IS_FRAMEBUFFERSINK (framebuffersink));

  sprintf(s, "%d frames rendered, %d from system memory, %d from video memory",
      framebuffersink->stats_video_frames_video_memory +
      framebuffersink->stats_overlay_frames_video_memory +
      framebuffersink->stats_video_frames_system_memory +
      framebuffersink->stats_overlay_frames_system_memory,
      framebuffersink->stats_video_frames_system_memory +
      framebuffersink->stats_overlay_frames_system_memory,
      framebuffersink->stats_video_frames_video_memory +
      framebuffersink->stats_overlay_frames_video_memory);
  GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink, s);

  if (framebuffersink->open_hardware_success)
    klass->close_hardware (framebuffersink);

  g_mutex_lock (&framebuffersink->flow_lock);

  gst_framebuffersink_pan_display(framebuffersink, 0, 0);

  if (munmap (framebuffersink->framebuffer, framebuffersink->framebuffer_map_size))
    return FALSE;

  if (framebuffersink->use_buffer_pool) {
    if (framebuffersink->pool) {
      gst_object_unref (framebuffersink->pool);
      framebuffersink->pool = NULL;
    }
  }

  close (framebuffersink->fd);
  g_free (framebuffersink->device);

  g_mutex_unlock (&framebuffersink->flow_lock);

  g_mutex_clear (&framebuffersink->flow_lock);

  if (framebuffersink->use_graphics_mode) {
    int kd_fd;
    kd_fd = open ("/dev/tty0", O_RDWR);
    ioctl (kd_fd, KDSETMODE, framebuffersink->saved_kd_mode);
    close (kd_fd);
  }

  return TRUE;
}

/* In non-overlay mode, there are two different show frame functions, */
/* one copying frames from memory to video memory, and one that just  */
/* pans to the frame that has already been  streamed into video memory. */

static GstFlowReturn
gst_framebuffersink_show_frame_memcpy (GstFramebufferSink *framebuffersink, GstBuffer *buffer) {
  GstMapInfo mapinfo = GST_MAP_INFO_INIT;
  GstMemory *mem;

  mem = gst_buffer_get_memory(buffer, 0);
  gst_memory_map(mem, &mapinfo, GST_MAP_READ);
  /* When not using page flipping, wait for vsync before copying. */
  if (framebuffersink->nu_framebuffers_used == 1 && framebuffersink->vsync)
    gst_framebuffersink_wait_for_vsync(framebuffersink);
  gst_framebuffersink_put_image_memcpy(framebuffersink, mapinfo.data);
  /* When using page flipping, wait for vsync after copying and then flip. */
  if (framebuffersink->nu_framebuffers_used >= 2 && framebuffersink->vsync) {
    if (!framebuffersink->pan_does_vsync)
      gst_framebuffersink_wait_for_vsync(framebuffersink);
    gst_framebuffersink_pan_to_framebuffer(framebuffersink, framebuffersink->current_framebuffer_index);
    framebuffersink->current_framebuffer_index++;
    if (framebuffersink->current_framebuffer_index >= framebuffersink->nu_framebuffers_used)
      framebuffersink->current_framebuffer_index = 0;
  }

  gst_memory_unmap(mem, &mapinfo);
  gst_memory_unref(mem);

  framebuffersink->stats_video_frames_system_memory++;

  return GST_FLOW_OK;
}

/* This show frame function can deal with both video memory buffers */
/* that require a pan and with regular buffers that need to be memcpy-ed. */

static GstFlowReturn
gst_framebuffersink_show_frame_buffer_pool (GstFramebufferSink * framebuffersink,
GstBuffer * buf)
{
  GstMemory *mem;
  GstMapInfo mapinfo;

  mem = gst_buffer_get_memory (buf, 0);
  if (!mem)
    goto invalid_memory;

  if (gst_framebuffersink_is_video_memory (mem)) {
    /* This a video memory buffer. */
    uint8_t *framebuffer_address;
    if (!gst_memory_map(mem, &mapinfo, 0)) {
      GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, "gst_memory_map video memory failed");
      gst_memory_unref (mem);
      return GST_FLOW_ERROR;
    }
    framebuffer_address = mapinfo.data;

#if 0
    {
    char s[80];
    g_sprintf(s, "Video memory buffer encountered (%p)", (guintptr) framebuffer_address);
    GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink, s);
    }
#endif

    gst_framebuffersink_put_image_pan(framebuffersink, framebuffer_address);

    gst_memory_unmap(mem, &mapinfo);

    gst_memory_unref(mem);

    framebuffersink->stats_video_frames_video_memory++;

    return GST_FLOW_OK;
  } else {
    /* This is a normal memory buffer (system memory). */
#if 0
    GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink, "Non-video memory buffer encountered");
#endif
    gst_memory_unref(mem);

    return gst_framebuffersink_show_frame_memcpy(framebuffersink, buf);
  }

invalid_memory:
    GST_ERROR_OBJECT (framebuffersink, "Show frame called with invalid memory buffer");
    return GST_FLOW_ERROR;
}

static GstFlowReturn
gst_framebuffersink_show_frame_overlay (GstFramebufferSink * framebuffersink,
GstBuffer * buf)
{
  GstFramebufferSinkClass *klass = GST_FRAMEBUFFERSINK_GET_CLASS (framebuffersink);
  GstMemory *mem;
  GstMapInfo mapinfo;

  mem = gst_buffer_get_memory (buf, 0);
  if (!mem)
    goto invalid_memory;

  if (gst_framebuffersink_is_video_memory (mem)) {
    /* This a video memory buffer. */
    uint8_t *framebuffer_address;
    if (!gst_memory_map(mem, &mapinfo, 0)) {
      GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, "gst_memory_map video memory failed");
      gst_memory_unref (mem);
      return GST_FLOW_ERROR;
    }
    framebuffer_address = mapinfo.data;

#if 0
    {
    char s[80];
    g_sprintf(s, "Video memory overlay buffer encountered (%p), mem = %p",
        (guintptr) framebuffer_address, mem);
    GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink, s);
    }
#endif

    /* Wait for vsync before changing the overlay address. */
    if (framebuffersink->vsync)
      gst_framebuffersink_wait_for_vsync(framebuffersink);
    klass->show_overlay(framebuffersink, framebuffer_address - framebuffersink->framebuffer);

    gst_memory_unmap(mem, &mapinfo);

    gst_memory_unref (mem);

    framebuffersink->stats_overlay_frames_video_memory++;

    return GST_FLOW_OK;
  } else {
    /* This is a normal memory buffer (system memory), but it is */
    /* overlay data. */

#if 0
    {
    char s[80];
    g_sprintf(s, "Non-video memory overlay buffer encountered, mem = %p",
        mem);
    GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink, s);
    }
#endif

    gst_memory_map(mem, &mapinfo, GST_MAP_READ);

    if (framebuffersink->use_buffer_pool) {
      /* When using a buffer pool in video memory, being requested to show an overlay */
      /* frame from system memory poses a bit of problem. We need to allocate a temporary video */
      /* memory area to store the overlay frame and show it. */
      GstMemory *vmem;
      vmem = gst_framebuffersink_video_memory_alloc(mapinfo.size, framebuffersink->overlay_alignment);
      gst_framebuffersink_put_overlay_image_memcpy (framebuffersink, vmem, mapinfo.data);
      gst_framebuffersink_video_memory_free (vmem);

      framebuffersink->stats_video_frames_system_memory++;
      return GST_FLOW_OK;
    }

    /* Copy the image into video memory in one of the slots after the first screen. */
    gst_framebuffersink_put_overlay_image_memcpy(framebuffersink,
        framebuffersink->overlays[framebuffersink->current_overlay_index], mapinfo.data);
    framebuffersink->current_overlay_index++;
    if (framebuffersink->current_overlay_index >= framebuffersink->nu_overlays_used)
      framebuffersink->current_overlay_index = 0;

    gst_memory_unmap(mem, &mapinfo);
    gst_memory_unref(mem);

    framebuffersink->stats_overlay_frames_system_memory++;

    return GST_FLOW_OK;
  }

invalid_memory:
    GST_ERROR_OBJECT (framebuffersink, "Show frame called with invalid memory buffer");
    return GST_FLOW_ERROR;
}

static GstFlowReturn
gst_framebuffersink_show_frame (GstVideoSink * vsink, GstBuffer * buf)
{
  GstFramebufferSink *framebuffersink = GST_FRAMEBUFFERSINK (vsink);
  GstFlowReturn res;

  if (framebuffersink->use_hardware_overlay)
    res = gst_framebuffersink_show_frame_overlay(framebuffersink, buf);
  else if (framebuffersink->use_buffer_pool)
    res = gst_framebuffersink_show_frame_buffer_pool(framebuffersink, buf);
  else
    res = gst_framebuffersink_show_frame_memcpy(framebuffersink, buf);
  return res;
}

static gboolean
gst_framebuffersink_set_buffer_pool_query_answer (GstFramebufferSink *framebuffersink,
GstQuery *query, GstBufferPool *pool, GstCaps *caps, GstVideoInfo *info)
{
    GstStructure *config;
    GstAllocator *allocator;
    GstAllocationParams allocation_params;
    gsize size, extra_size;
    int n;

    GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, "Providing video memory buffer pool");

    size = info->size;
    n = framebuffersink->nu_framebuffers_used;
    if (framebuffersink->use_hardware_overlay)
      n = framebuffersink->nu_overlays_used;

    config = gst_buffer_pool_get_config (pool);
#ifdef HALF_POOLS
    gst_buffer_pool_config_set_params (config, caps, size, n / 2, n / 2);
#else
    gst_buffer_pool_config_set_params (config, caps, size, n, n);
#endif
    if (!gst_buffer_pool_set_config (pool, config))
      return FALSE;

    /* Add the video memory allocator. */
    allocator = gst_allocator_find ("framebuffersink_video_memory");
    gst_framebuffersink_allocation_params_init (framebuffersink, &allocation_params);
    gst_query_add_allocation_param (query,
        allocator,
        &allocation_params);

    gst_object_unref (allocator);
#if 0
    /* Add the default allocator. */
    allocator = gst_allocator_find (NULL);
    gst_allocation_params_init (&allocation_params);
    gst_query_add_allocation_param (query,
        allocator,
        &allocation_params);
    gst_object_unref (allocator);
#endif

#ifdef HALF_POOLS
    gst_query_add_allocation_pool (query, pool, size, n / 2, n / 2);
#else
    gst_query_add_allocation_pool (query, pool, size, n, n);
#endif

    gst_object_unref (pool);

    {
      char s[80];
      sprintf(s, "propose_allocation: size = %.2lf MB, %d buffers",
          (double) size / (1024 * 1024), n);
      GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, s);
    }

#ifdef EXTRA_DEBUG
    GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink,
      "propose_allocation: provide our video memory buffer pool");
#endif

  return TRUE;
}

/* This function is called by upstream asking for the buffer allocation */
/* configuration. We need to answer with our own video memory-based */
/* buffer configuration, when it is enabled. */

static gboolean
gst_framebuffersink_propose_allocation (GstBaseSink * bsink, GstQuery * query)
{
  GstFramebufferSink *framebuffersink = GST_FRAMEBUFFERSINK (bsink);
  GstBufferPool *pool;
  GstStructure *config;
  GstCaps *caps;
  GstVideoInfo info;
  guint size;
  gboolean need_pool;
  GstAllocator *allocator;

  GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, "propose_allocation called");

  gst_query_parse_allocation (query, &caps, &need_pool);

  if (caps == NULL)
    goto no_caps;

//  if (!framebuffersink->silent) {
//    g_print ("propose_allocation: caps: %" GST_PTR_FORMAT "\n", caps);

  if (!gst_video_info_from_caps (&info, caps))
    goto invalid_caps;

  /* Take a look at our pre-initialized pool in video memory. */
  GST_OBJECT_LOCK (framebuffersink);
  pool = framebuffersink->pool ? gst_object_ref (framebuffersink->pool) : NULL;
  GST_OBJECT_UNLOCK (framebuffersink);

#ifdef USE_SAME_POOL
  if (pool)
    if (gst_buffer_pool_is_active (pool)) {
      int n;
      GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, "Providing query with already activated pool");
      n = framebuffersink->nu_framebuffers_used;
      if (framebuffersink->use_hardware_overlay)
        n = framebuffersink->nu_overlays_used;
      gst_query_add_allocation_pool (query, pool, info.size, n, n);
      return TRUE;
    }
#endif

  /* If we had a buffer pool in video memory and it has been allocated, */
  /* we can't easily provide regular system memory buffers because */
  /* due to the difficulty of handling page flips correctly. */
  if (framebuffersink->use_buffer_pool && pool == NULL) {
    GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink,
        "propose_allocation: Already provided video memory buffer pool");
  }

  if (pool != NULL) {
    GstCaps *pcaps;
    guintptr start_offset;

    /* We have a pool, check the caps. */
    GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, "check existing pool caps");
    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_get_params (config, &pcaps, &size, NULL, NULL);

    if (!gst_caps_is_equal (caps, pcaps)) {
      GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, "pool has different caps");
      /* Different caps, we can't use our pool. */
      gst_object_unref (pool);
      pool = NULL;
    }
    gst_structure_free (config);
  }

#ifndef USE_SAME_POOL
  if (pool) {
    GST_OBJECT_LOCK (framebuffersink);
    framebuffersink->pool = NULL;
    GST_OBJECT_UNLOCK (framebuffersink);
  }
#endif

#ifdef MULTIPLE_VIDEO_MEMORY_POOLS
  if (framebuffersink->use_buffer_pool && pool == NULL && need_pool &&
  gst_framebuffersink_video_memory_get_available() >= info.size * 2 &&
  framebuffersink->use_hardware_overlay) {
    /* Try to provide (another) pool from video memory. */

    pool = gst_framebuffersink_allocate_buffer_pool (framebuffersink, caps, &info);
    if (!pool)
      return FALSE;
    pool = gst_object_ref (pool);
  }
#endif

  /* At this point if pool is not NULL we have a video memory pool */
  /* to provide. */
  if (pool != NULL) {
    if (!gst_framebuffersink_set_buffer_pool_query_answer (framebuffersink, query, pool, caps, &info))
      goto config_failed;

    goto end;
  }

  if (pool == NULL && need_pool) {
    /* Provide a regular system memory buffer pool. */

    GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, "create new system memory pool");
    pool = gst_video_buffer_pool_new ();

    size = info.size;

    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_set_params (config, caps, info.size, 0, 0);
    if (!gst_buffer_pool_set_config (pool, config))
      goto config_failed;

    gst_query_add_allocation_pool (query, pool, size, 0, 0);
    gst_object_unref (pool);
  }

end:
  /* we also support various metadata */
//  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);
//  gst_query_add_allocation_meta (query, GST_VIDEO_CROP_META_API_TYPE, NULL);

  return TRUE;

  /* ERRORS */
no_caps:
  {
    GST_FRAMEBUFFERSINK_INFO_OBJECT (bsink, "no caps specified");
    return FALSE;
  }
invalid_caps:
  {
    GST_FRAMEBUFFERSINK_INFO_OBJECT (bsink, "invalid caps specified");
    return FALSE;
  }
config_failed:
  {
    GST_FRAMEBUFFERSINK_INFO_OBJECT (bsink, "failed setting config");
    gst_object_unref (pool);
    return FALSE;
  }
}

GType
gst_framebuffersink_get_type (void)
{
  static GType framebuffersink_type = 0;

  if (!framebuffersink_type) {
    static const GTypeInfo framebuffersink_info = {
      sizeof (GstFramebufferSinkClass),
      gst_framebuffersink_base_init,
      NULL,
      (GClassInitFunc) gst_framebuffersink_class_init,
      NULL,
      NULL,
      sizeof (GstFramebufferSink),
      0,
      (GInstanceInitFunc) gst_framebuffersink_init,
    };

    framebuffersink_type = g_type_register_static( GST_TYPE_VIDEO_SINK,
        "GstFramebufferSink", &framebuffersink_info, 0);
  }

  return framebuffersink_type;
}

/* Video memory. */

typedef struct
{
  GstMemory mem;
  gpointer data;

} GstFramebufferSinkVideoMemory;

static GstMemory *
gst_framebuffersink_video_memory_allocator_alloc (GstAllocator * allocator, gsize size, GstAllocationParams * params)
{
  return gst_framebuffersink_video_memory_alloc (size, params->align);
}

static void
gst_framebuffersink_video_memory_allocator_free (GstAllocator * allocator, GstMemory * mem)
{
  GstFramebufferSinkVideoMemory *vmem = (GstFramebufferSinkVideoMemory *) mem;

//  g_print("video_memory_allocator_free called, address = %p\n", vmem->data);

  gst_framebuffersink_video_memory_free (mem);

  GST_DEBUG ("%p: freed", vmem);
}

static gpointer
gst_framebuffersink_video_memory_map (GstFramebufferSinkVideoMemory * mem, gsize maxsize, GstMapFlags flags)
{
  gpointer res;

//  g_print ("video_memory_map called, mem = %p, maxsize = %d, flags = %d, data = %p\n", mem,
//      maxsize, flags, mem->data);

//  if (flags & GST_MAP_READ)
//    g_print ("Mapping video memory for reading is slow.\n");

  return mem->data;

  while (TRUE) {
    if ((res = g_atomic_pointer_get (&mem->data)) != NULL)
      break;

    res = g_malloc (maxsize);

    if (g_atomic_pointer_compare_and_exchange (&mem->data, NULL, res))
      break;

    g_free (res);
  }

  GST_DEBUG ("%p: mapped %p", mem, res);

  return res;
}

static gboolean
gst_framebuffersink_video_memory_unmap (GstFramebufferSinkVideoMemory * mem)
{
  GST_DEBUG ("%p: unmapped", mem);
  return TRUE;
}

static GstFramebufferSinkVideoMemory *
gst_framebuffersink_video_memory_share (GstFramebufferSinkVideoMemory * mem, gssize offset, gsize size)
{
  GstFramebufferSinkVideoMemory *sub;
  GstMemory *parent;

  GST_DEBUG ("%p: share %" G_GSSIZE_FORMAT " %" G_GSIZE_FORMAT, mem, offset,
      size);

  /* find the real parent */
  if ((parent = mem->mem.parent) == NULL)
    parent = (GstMemory *) mem;

  if (size == -1)
    size = mem->mem.size - offset;

  sub = g_slice_new (GstFramebufferSinkVideoMemory);
  /* the shared memory is always readonly */
  gst_memory_init (GST_MEMORY_CAST (sub), GST_MINI_OBJECT_FLAGS (parent) |
      GST_MINI_OBJECT_FLAG_LOCK_READONLY, mem->mem.allocator, parent,
      mem->mem.maxsize, mem->mem.align, mem->mem.offset + offset, size);

  /* install pointer */
  sub->data = gst_framebuffersink_video_memory_map (mem, mem->mem.maxsize, GST_MAP_READ);

  return sub;
}

typedef struct
{
  GstAllocator parent;
  gpointer framebuffer;
  gsize framebuffer_size;
  /* The lowest non-allocated offset. */
  gsize end_marker;
  /* The amount of video memory allocated. */
  gsize total_allocated;
  /* Maintain a sorted linked list of allocated memory regions. */
  GList *chain;
} GstFramebufferSinkVideoMemoryAllocator;

typedef struct
{
  GstAllocatorClass parent_class;
} GstFramebufferSinkVideoMemoryAllocatorClass;

static GstFramebufferSinkVideoMemoryAllocator *video_memory_allocator;

GType gst_framebuffersink_video_memory_allocator_get_type (void);
G_DEFINE_TYPE (GstFramebufferSinkVideoMemoryAllocator, gst_framebuffersink_video_memory_allocator, GST_TYPE_ALLOCATOR);

static void
gst_framebuffersink_video_memory_allocator_class_init (GstFramebufferSinkVideoMemoryAllocatorClass * klass) {
  GObjectClass * g_object_class = G_OBJECT_CLASS (klass);
  GstAllocatorClass * allocator_class = GST_ALLOCATOR_CLASS (klass);

  allocator_class->alloc = gst_framebuffersink_video_memory_allocator_alloc;
  allocator_class->free = gst_framebuffersink_video_memory_allocator_free;
}

static void
gst_framebuffersink_video_memory_allocator_init (GstFramebufferSinkVideoMemoryAllocator *video_memory_allocator) {
  GstAllocator * alloc = GST_ALLOCATOR_CAST (video_memory_allocator);

  alloc->mem_type = "framebuffersink_video_memory";
  alloc->mem_map = (GstMemoryMapFunction) gst_framebuffersink_video_memory_map;
  alloc->mem_unmap = (GstMemoryUnmapFunction) gst_framebuffersink_video_memory_unmap;
  alloc->mem_share = (GstMemoryShareFunction) gst_framebuffersink_video_memory_share;
}

static void
gst_framebuffersink_video_memory_init (gpointer framebuffer, gsize framebuffer_size) {
  video_memory_allocator = g_object_new (gst_framebuffersink_video_memory_allocator_get_type (), NULL);
  video_memory_allocator->framebuffer = framebuffer;
  video_memory_allocator->framebuffer_size = framebuffer_size;
  video_memory_allocator->total_allocated = 0;
  video_memory_allocator->end_marker = 0;
  video_memory_allocator->chain = NULL;
  gst_allocator_register ("framebuffersink_video_memory", gst_object_ref (video_memory_allocator) );
}

typedef struct {
  gpointer framebuffer_address;
  gsize size;
} ChainEntry;

static GstMemory *
gst_framebuffersink_video_memory_alloc (gsize size, int align)
{
  GstFramebufferSinkVideoMemory *mem;
  int align_bytes;
  guintptr framebuffer_offset;
  GList *chain;
  ChainEntry *chain_entry;

  GST_DEBUG ("alloc frame %u", size);

  GST_OBJECT_LOCK (video_memory_allocator);

  align_bytes = ALIGNMENT_GET_ALIGN_BYTES(video_memory_allocator->end_marker, align);
  framebuffer_offset = video_memory_allocator->end_marker + align_bytes;

  if (video_memory_allocator->end_marker + align_bytes + size > video_memory_allocator->framebuffer_size) {
      /* When we can't just provide memory from beyond the highest allocated address, */
      /* we to have traverse our chain to find a free spot. */
      ChainEntry *previous_entry;
      chain = video_memory_allocator->chain;
      previous_entry = NULL;
      while (chain != NULL) {
        ChainEntry *entry = chain->data;
        gsize gap_size;
        gpointer gap_start;
        if (previous_entry == NULL)
          gap_start = video_memory_allocator->framebuffer;
        else
          gap_start = previous_entry->framebuffer_address + previous_entry->size;
        gap_size = entry->framebuffer_address - gap_start;
        align_bytes = ALIGNMENT_GET_ALIGN_BYTES(gap_start - video_memory_allocator->framebuffer, align);
        if (gap_size >= align_bytes + size) {
          /* We found a gap large enough to fit the requested size. */
          framebuffer_offset = gap_start + align_bytes - video_memory_allocator->framebuffer;
          break;
        }
        previous_entry = entry;
        chain = g_list_next (chain);
      }
      if (chain == NULL) {
        GST_ERROR_OBJECT (video_memory_allocator, "Out of video memory");
        GST_OBJECT_UNLOCK (video_memory_allocator);
        return NULL;
      }
  }

  mem = g_slice_new (GstFramebufferSinkVideoMemory);

  gst_memory_init (GST_MEMORY_CAST (mem), 0, (GstAllocator *)video_memory_allocator, NULL,
      size, align, 0, size);

  mem->data = video_memory_allocator->framebuffer + framebuffer_offset;
  if (framebuffer_offset + size > video_memory_allocator->end_marker)
    video_memory_allocator->end_marker = framebuffer_offset + size;
  video_memory_allocator->total_allocated += size;

  /* Insert the allocated area into the chain. */

  /* Find the first entry whose framebuffer address is greater. */
  chain = video_memory_allocator->chain;
  while (chain != NULL) {
    ChainEntry *entry = chain->data;
    if (entry->framebuffer_address > mem->data)
        break;
    chain = g_list_next (chain);
  }
  /* Insert the new entry (if chain is NULL, the entry will be appended at the end). */
  chain_entry = g_slice_new (ChainEntry);
  chain_entry->framebuffer_address = mem->data;
  chain_entry->size = size;
  video_memory_allocator->chain = g_list_insert_before (video_memory_allocator->chain,
      chain, chain_entry);

  GST_OBJECT_UNLOCK(video_memory_allocator);

//  g_print ("Allocated video memory buffer of size %d at %p, align %d, mem = %p\n", size,
//      mem->data, align, mem);

  return (GstMemory *) mem;
}

static void gst_framebuffersink_video_memory_free (GstMemory *mem)
{
  GstFramebufferSinkVideoMemory *vmem = (GstFramebufferSinkVideoMemory *) mem;
  GList *chain;

  GST_OBJECT_LOCK (video_memory_allocator);

  chain = video_memory_allocator->chain;

  while (chain != NULL) {
    ChainEntry *entry = chain->data;
    if (entry->framebuffer_address == vmem->data && entry->size == mem->size) {
      /* Delete this entry. */
      g_slice_free (ChainEntry, entry);
      /* Update the end marker. */
      if (g_list_next (chain) == NULL) {
        GList *previous = g_list_previous (chain);
        if (previous == NULL)
          video_memory_allocator->end_marker = 0;
        else {
          ChainEntry *previous_entry = previous->data;
          video_memory_allocator->end_marker = previous_entry->framebuffer_address +
              previous_entry->size - video_memory_allocator->framebuffer;
        }
      }
      video_memory_allocator->chain = g_list_delete_link (video_memory_allocator->chain, chain);
      video_memory_allocator->total_allocated -= mem->size;
      GST_OBJECT_UNLOCK (video_memory_allocator);
      g_slice_free (GstFramebufferSinkVideoMemory, vmem);
      return;
    }
    chain = g_list_next (chain);
  }

  GST_OBJECT_UNLOCK (video_memory_allocator);

  GST_ERROR_OBJECT (video_memory_allocator, "video_memory_free failed");
}

static gboolean
gst_framebuffersink_is_video_memory (GstMemory * mem)
{
  return mem->allocator == (GstAllocator *)video_memory_allocator;
}

static gsize
gst_framebuffersink_video_memory_get_available (void)
{
  return video_memory_allocator->framebuffer_size - video_memory_allocator->total_allocated;
}
