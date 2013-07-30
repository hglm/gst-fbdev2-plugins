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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:class-GstFramebufferSink
 *
 * The GstFramebufferSink class implements an optimized video sink
 * for the Linux console framebuffer. It used as the basis for the
 * fbdev2sink plugin. It can write directly into video memory with
 * page flipping support, and should be usable by a wide variety of
 * devices. The class can derived for device-specific implementations
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
#include <gst/video/video.h>
#include <gst/video/video-info.h>
#include "gstframebuffersink.h"

GST_DEBUG_CATEGORY_STATIC (gst_framebuffersink_debug_category);
#define GST_CAT_DEFAULT gst_framebuffersink_debug_category

/* Inline function to produce both normal message and debug info. */
static inline void GST_FRAMEBUFFERSINK_INFO_OBJECT (GstFramebufferSink * framebuffersink,
const gchar *message) {
  if (!framebuffersink->silent) g_print (message); g_print(".\n");
  GST_INFO_OBJECT (framebuffersink, message);
}

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
static void gst_framebuffersink_get_supported_overlay_types (GstFramebufferSink *framebuffersink,
    uint8_t *types);
static gboolean gst_framebuffersink_open_hardware (GstFramebufferSink *framebuffersink);
static void gst_framebuffersink_close_hardware (GstFramebufferSink *framebuffersink);

/* Local functions. */
static gboolean gst_framebuffersink_open_device (GstFramebufferSink * sink);
static void gst_buffer_print(GstFramebufferSink *framebuffersink, GstBuffer *buf);

#define GST_MEMORY_FLAG_FRAMEBUFFERSINK GST_MEMORY_FLAG_LAST

static gboolean gst_framebuffersink_buffer_allocation_table_configure (GstFramebufferSink * sink, gsize size, guintptr offset);
static GstFramebufferSinkAllocator *gst_framebuffersink_allocator_new (GstFramebufferSink * sink);

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
  PROP_NATIVE_RESOLUTION,
  PROP_PRESERVE_PAR,
  PROP_CLEAR,
  PROP_FRAMES_PER_SECOND,
  PROP_BUFFER_POOL,
  PROP_VSYNC,
  PROP_FLIP_BUFFERS,
  PROP_GRAPHICS_MODE,
  PROP_PAN_DOES_VSYNC,
  PROP_USE_HARDWARE_OVERLAY,
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

typedef struct {
  GstVideoFormat format;
  const char *caps_string;
} GstFramebufferSinkStaticOverlayFormatData;

static GstFramebufferSinkStaticOverlayFormatData overlay_format_data[2] = {
  { GST_VIDEO_FORMAT_I420, "I420" },
  { GST_VIDEO_FORMAT_BGRx, "BGRx" },
};

/* Class initialization. */

static void
gst_framebuffersink_class_init (GstFramebufferSinkClass* klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseSinkClass *base_sink_class = GST_BASE_SINK_CLASS (klass);
  GstVideoSinkClass *video_sink_class = (GstVideoSinkClass *) klass;

  gobject_class->set_property = gst_framebuffersink_set_property;
  gobject_class->get_property = gst_framebuffersink_get_property;

  /* define properties */
  g_object_class_install_property (gobject_class, PROP_SILENT,
    g_param_spec_boolean ("silent", "silent",
			  "Whether to be very verbose or not",
			  FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_DEVICE,
      g_param_spec_string ("device", "device",
          "The framebuffer device", "/dev/fb0",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_ACTUAL_WIDTH,
    g_param_spec_int ("actual-width", "actual-width",
			  "Actual width of the video window source", 0, G_MAXINT,
			  0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_ACTUAL_HEIGHT,
    g_param_spec_int ("actual-height", "actual-height",
			  "Actual height of the video window source", 0, G_MAXINT,
			  0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_REQUESTED_WIDTH,
    g_param_spec_int ("width", "width",
			  "Requested width of the video output window (0 = auto)",
                          0, G_MAXINT,
			  0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_REQUESTED_HEIGHT,
    g_param_spec_int ("height", "height",
			  "Requested height of the video output window (0 = auto)",
                          0, G_MAXINT,
			  0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_SCREEN_WIDTH,
    g_param_spec_int ("screen-width", "screen-width",
			  "Width of the screen", 1, G_MAXINT,
			  1, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_SCREEN_HEIGHT,
    g_param_spec_int ("screen-height", "screen-height",
			  "Height of the screen", 1, G_MAXINT,
			  1, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_WIDTH_BEFORE_SCALING,
    g_param_spec_int ("width-before-scaling", "width-before-scaling",
			  "Requested width of the video source when using hardware scaling "
                          "(0 = use default source width)",
                          0, G_MAXINT,
			  0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_HEIGHT_BEFORE_SCALING,
    g_param_spec_int ("height-before-scaling", "height-before-scaling",
			  "Requested height of the video source when using hardware scaling "
                          "(0 = use default source height)",
                          0, G_MAXINT,
			  0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_NATIVE_RESOLUTION,
    g_param_spec_boolean ("native-resolution", "native-resolution",
			  "Force full-screen video output resolution "
                          "(equivalent to setting width and "
                          "height to screen dimensions)",
                          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PRESERVE_PAR,
    g_param_spec_boolean ("preserve-par", "preserve-par",
			  "Preserve the pixel aspect ratio by adding black boxes if necessary",
                          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_CLEAR,
    g_param_spec_boolean ("clear", "clear",
			  "Clear the screen to black before playing",
                          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_FRAMES_PER_SECOND,
    g_param_spec_int ("fps", "fps",
			  "Frames per second (0 = auto)", 0, G_MAXINT,
			  0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_BUFFER_POOL,
    g_param_spec_boolean ("buffer-pool", "buffer-pool",
			  "Use a custom buffer pool and write directly to the screen if possible",
                          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_VSYNC,
    g_param_spec_boolean ("vsync", "vsync",
			  "Sync to vertical retrace. Especially useful with buffer-pool=true.",
                          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_FLIP_BUFFERS,
    g_param_spec_int ("flip-buffers", "flip-buffers",
			  "The maximum number of buffers in video memory to use for page flipping. "
                          "Page flipping is disabled when set to 1. Use of a buffer-pool requires "
                          "at least 2 buffers. Default is 0 (auto).",
                          0, G_MAXINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_GRAPHICS_MODE,
    g_param_spec_boolean ("graphics-mode", "graphics-mode",
			  "Set the console to KDGRAPHICS mode. This eliminates interference from "
                          "text output and the cursor but can result in textmode not being restored "
                          "in case of a crash. Use with care.",
                          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PAN_DOES_VSYNC,
    g_param_spec_boolean ("pan-does-vsync", "pan-does-vsync",
			  "When set to true this property hints that the kernel display pan function "
                          "performs vsync automatically or otherwise doesn't need a vsync call "
                          "around it.",
                          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_USE_HARDWARE_OVERLAY,
    g_param_spec_boolean ("hardware-overlay", "hardware-overlay",
                          "Use hardware overlay scaler if available. Not available in the default "
                          "fbdev2sink but may be available in derived sinks.",
                          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  base_sink_class->start = GST_DEBUG_FUNCPTR (gst_framebuffersink_start);
  base_sink_class->stop = GST_DEBUG_FUNCPTR (gst_framebuffersink_stop);
  base_sink_class->get_caps = GST_DEBUG_FUNCPTR (gst_framebuffersink_get_caps);
  base_sink_class->set_caps = GST_DEBUG_FUNCPTR (gst_framebuffersink_set_caps);
  base_sink_class->propose_allocation = GST_DEBUG_FUNCPTR (gst_framebuffersink_propose_allocation);
  video_sink_class->show_frame = GST_DEBUG_FUNCPTR (gst_framebuffersink_show_frame);
  klass->open_hardware = GST_DEBUG_FUNCPTR (gst_framebuffersink_open_hardware);
  klass->close_hardware = GST_DEBUG_FUNCPTR (gst_framebuffersink_close_hardware);
  klass->get_supported_overlay_types = GST_DEBUG_FUNCPTR (gst_framebuffersink_get_supported_overlay_types);
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
  framebuffersink->allocator = NULL;

  framebuffersink->framebuffer = NULL;
  framebuffersink->device = NULL;
  framebuffersink->pool = NULL;
  framebuffersink->buffer_allocation_table = NULL;
  framebuffersink->have_caps = FALSE;
  framebuffersink->adjusted_dimensions = FALSE;

  /* Set the initial values of the properties.*/
  framebuffersink->device = g_strdup("/dev/fb0");
  framebuffersink->videosink.width = 0;
  framebuffersink->videosink.height = 0;
  framebuffersink->silent = FALSE;
  framebuffersink->native_resolution = FALSE;
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

/* Default implementation of get_supported_overlay_types: none supported. */

static void
gst_framebuffersink_get_supported_overlay_types (GstFramebufferSink *framebuffersink,
    uint8_t *types)
{
  int i;
  for (i = GST_FRAMEBUFFERSINK_OVERLAY_TYPE_FIRST; i <=
      GST_FRAMEBUFFERSINK_OVERLAY_TYPE_LAST; i++) {
    types[i] = 0;
  }
}

static gboolean
gst_framebuffersink_video_format_supported_by_overlay (GstFramebufferSink *framebuffersink, GstVideoFormat format) {
  if (format == GST_VIDEO_FORMAT_BGRx &&
      framebuffersink->hardware_overlay_types_supported[GST_FRAMEBUFFERSINK_OVERLAY_TYPE_BGRX32])
    return TRUE;
  if (format == GST_VIDEO_FORMAT_I420 &&
      framebuffersink->hardware_overlay_types_supported[GST_FRAMEBUFFERSINK_OVERLAY_TYPE_I420])
    return TRUE;
  return FALSE;
}

static GstFramebufferSinkOverlayType
gst_framebuffersink_video_format_to_overlay_type (GstVideoFormat format) {
  if (format == GST_VIDEO_FORMAT_BGRx)
    return GST_FRAMEBUFFERSINK_OVERLAY_TYPE_BGRX32;
  if (format == GST_VIDEO_FORMAT_I420)
    return GST_FRAMEBUFFERSINK_OVERLAY_TYPE_I420;

  return GST_FRAMEBUFFERSINK_OVERLAY_TYPE_NONE;
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
    case PROP_NATIVE_RESOLUTION:
      framebuffersink->native_resolution = g_value_get_boolean (value);
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
    case PROP_NATIVE_RESOLUTION:
      g_value_set_boolean (value, framebuffersink->native_resolution);
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

  /* map the framebuffer */
  framebuffersink->framebuffer = mmap (0, framebuffersink->fixinfo.smem_len,
      PROT_WRITE, MAP_SHARED, framebuffersink->fd, 0);
  if (framebuffersink->framebuffer == MAP_FAILED)
    goto err;

  framebuffersink->max_framebuffers = framebuffersink->fixinfo.smem_len /
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

  char *s = malloc(strlen(framebuffersink->device) + 128);
  g_sprintf(s, "Succesfully opened framebuffer device %s, "
      "pixel depth %d, dimensions %d x %d",
      framebuffersink->device,
      framebuffersink->bytespp * 8, framebuffersink->varinfo.xres,
      framebuffersink->varinfo.yres);
  GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink, s);
  free(s);
  if (framebuffersink->vsync)
      GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink, "vsync enabled");

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

  // Create a defaul framebuffer allocation table.
  gst_framebuffersink_buffer_allocation_table_configure (framebuffersink,
    framebuffersink->fixinfo.line_length * framebuffersink->varinfo.yres, (guintptr) 0);

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
  memset(framebuffersink->framebuffer +
      index * framebuffersink->fixinfo.line_length * framebuffersink->varinfo.yres,
      0, framebuffersink->fixinfo.line_length * framebuffersink->varinfo.yres);
}

static void
gst_framebuffersink_put_image_memcpy (GstFramebufferSink * framebuffersink, uint8_t * src)
{
  guint8  *dest;
  guintptr dest_stride;
  int i;

  dest = framebuffersink->framebuffer +
      framebuffersink->current_framebuffer_index * framebuffersink->fixinfo.line_length *
      framebuffersink->varinfo.yres +
      framebuffersink->cy * framebuffersink->fixinfo.line_length + framebuffersink->cx
      * framebuffersink->bytespp;
  dest_stride = framebuffersink->fixinfo.line_length;
  if (framebuffersink->video_width_in_bytes == dest_stride)
      memcpy(dest, src, dest_stride * framebuffersink->lines);
  else
    for (i = 0; i < framebuffersink->lines; i++) {
      memcpy(dest, src, framebuffersink->video_width_in_bytes);
      src += framebuffersink->video_width_in_bytes;
      dest += dest_stride;
    }
  return;
}

static void
gst_framebuffersink_put_overlay_image_memcpy(GstFramebufferSink *framebuffersink,
    uint8_t *src)
{
  GstFramebufferSinkClass *klass = GST_FRAMEBUFFERSINK_GET_CLASS (framebuffersink);
  uint8_t *framebuffer_address;
  framebuffer_address = framebuffersink->framebuffer + framebuffersink->fixinfo.line_length *
      framebuffersink->varinfo.yres + framebuffersink->info.size *
      framebuffersink->current_overlay_index;
  memcpy(framebuffer_address, src, framebuffersink->info.size);
  klass->show_overlay(framebuffersink, framebuffer_address - framebuffersink->framebuffer);
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

  if (!gst_framebuffersink_open_device(framebuffersink))
    return FALSE;

  framebuffersink->open_hardware_success = FALSE;
  if (framebuffersink->use_hardware_overlay) {
    framebuffersink->open_hardware_success = klass->open_hardware (framebuffersink);
    if (!framebuffersink->open_hardware_success)
      /* Disable any hardware acceleration features. */
      framebuffersink->use_hardware_overlay = FALSE;
  }

  if (framebuffersink->native_resolution) {
      framebuffersink->requested_video_width = framebuffersink->varinfo.xres;
      framebuffersink->requested_video_height = framebuffersink->varinfo.yres;
  }

  framebuffersink->current_framebuffer_index = 0;

  if (framebuffersink->use_buffer_pool) {
    framebuffersink->allocator = gst_framebuffersink_allocator_new(framebuffersink);
    framebuffersink->framebuffer_address_quark = g_quark_from_static_string ("framebuffer_address");
  }

  /* Reset overlay types. */
  gst_framebuffersink_get_supported_overlay_types(framebuffersink,
    framebuffersink->hardware_overlay_types_supported);
  /* Set overlay types if supported. */
  if (framebuffersink->use_hardware_overlay) {
    framebuffersink->current_overlay_index = 0;
    klass->get_supported_overlay_types (framebuffersink, framebuffersink->hardware_overlay_types_supported);
  }

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
  int i;
  GstCaps *framebuffer_caps;

  if (framebuffersink->framebuffer_format == GST_VIDEO_FORMAT_UNKNOWN)
    goto unknown_format;

  caps = gst_caps_new_empty();

  /* First add any specific overlay formats that are supported. */
  /* They will have precedence over the standard framebuffer format. */

  for (i = GST_FRAMEBUFFERSINK_OVERLAY_TYPE_FIRST; i <= GST_FRAMEBUFFERSINK_OVERLAY_TYPE_LAST;
      i++)
    if (framebuffersink->hardware_overlay_types_supported[i] &&
        overlay_format_data[i].format != framebuffersink->framebuffer_format) {
      GstCaps *overlay_caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING,
          overlay_format_data[i].caps_string, NULL);
      gst_caps_append(caps, overlay_caps);
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
  char s[80];

  GST_DEBUG_OBJECT (framebuffersink, "get_caps");

#if 1
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
    caps = gst_caps_ref(framebuffersink->caps);
    goto done_no_store;
  }

  /* Check whether upstream is reporting video dimensions and par. */
  no_par = TRUE;
  if (filter == NULL)
    n = 0;
  else
    n = gst_caps_get_size(filter);
  w = 0;
  h = 0;
  par_n = 0;
  par_d = 0;
  for (i = 0; i < n; i++) {
    GstStructure *str = gst_caps_get_structure(filter, i);
    gst_structure_get_int(str, "width", &w);
    gst_structure_get_int(str, "height", &h);
    if (gst_structure_has_field(str, "pixel-aspect-ratio")) {
      no_par = FALSE;
      gst_structure_get_fraction(str, "pixel-aspect-ratio", &par_n, &par_d);
    }
  }

  /* Set the caps to the stored ones if have them, otherwise generate default caps. */
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
        if (framebuffersink->requested_video_width != 0) {
          output_width = framebuffersink->requested_video_width;
          if (framebuffersink->requested_video_height != 0)
            /* Both requested width and height specified. */
            output_height = framebuffersink->requested_video_height;
          else
            output_height = (double) output_width / ratio;
        }
        else if (framebuffersink->requested_video_height != 0) {
          output_height = framebuffersink->requested_video_height;
          output_width = (double) output_height * ratio;
        }

        r = (double) output_width / output_height;
        if (r > ratio + 0.01)
          /* Insert black borders on the sides. */
        output_width = output_width * ratio / r;
        else if (r < ratio - 0.01)
          /* Insert black borders on the top and bottom. */
          output_height = output_height * r / ratio;

        if (output_width != w || output_height != h) {
          GstCaps *icaps;
          GstVideoInfo info;

          /* Intersect and set new output dimensions. */
          icaps = gst_caps_intersect (caps, filter);
          gst_caps_unref (caps);
          caps = icaps;

          /* If we are not using the hardware overlay, inform upstream to */
          /* scale to the new size. */
          gst_video_info_from_caps (&info, caps);
          if (!gst_framebuffersink_video_format_supported_by_overlay (framebuffersink,
              GST_VIDEO_INFO_FORMAT (&info))) {
            gst_caps_set_simple(caps,
                "width", G_TYPE_INT, output_width, NULL);
            gst_caps_set_simple(caps,
                "height", G_TYPE_INT, output_height, NULL);
            gst_caps_set_simple(caps, "pixel-aspect-ratio", GST_TYPE_FRACTION,
                par_n * output_width * h, par_d * output_height * w, NULL);
          }

          sprintf(s, "Preserve aspect ratio: Adjusted output dimensions to %d x %d",
              output_width, output_height);
          GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, s);

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

#if 1
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

/* This function is called from set_caps when we are configured with */
/* use_buffer_pool=true. */

static gboolean
gst_framebuffersink_allocate_buffer_pool (GstFramebufferSink * framebuffersink, GstCaps *caps,
GstVideoInfo *info) {
  GstStructure *config;
  GstBufferPool *newpool, *oldpool;
  GstAllocator * allocator;
  GstAllocationParams allocation_params;
  guintptr offset;
  int n;
  int i;
  char s[80];

  GST_DEBUG("allocate_buffer_pool, caps: %" GST_PTR_FORMAT, caps);

  offset = 0;
  /* When using hardware overlay, buffers are allocated starting after the first visible screen. */
  if (framebuffersink->use_hardware_overlay)
    offset = framebuffersink->fixinfo.line_length * framebuffersink->varinfo.yres;

  if (!gst_framebuffersink_buffer_allocation_table_configure(framebuffersink, info->size, offset)) {
    GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink,
        "Could not reconfigure framebuffer allocation table for new size");
    return FALSE;
  }

  g_mutex_lock (&framebuffersink->flow_lock);

  /* Create a new pool for the new configuration. */
  newpool = gst_buffer_pool_new ();

  config = gst_buffer_pool_get_config (newpool);

  n = framebuffersink->nu_framebuffers_used;
  if (framebuffersink->use_hardware_overlay)
    n = framebuffersink->nu_overlays_used;

  gst_buffer_pool_config_set_params (config, caps, info->size, n, n);

  if (framebuffersink->allocator == NULL)
    framebuffersink->allocator = gst_framebuffersink_allocator_new(framebuffersink);
  allocator = GST_ALLOCATOR (framebuffersink->allocator);
  allocation_params.flags = 0;
  /* Determine the minimum alignment of the framebuffer pages. */
  /* The minimum guaranteed alignment is word-aligned (align = 3). */
  for (i = 8; i <= 4096; i <<= 1)
    if (framebuffersink->fixinfo.line_length & (i - 1))
      break;
  allocation_params.align = (i >> 1) - 1;
  allocation_params.prefix = 0;
  allocation_params.padding = 0;
  gst_buffer_pool_config_set_allocator (config, allocator, &allocation_params);
  if (!gst_buffer_pool_set_config (newpool, config))
    goto config_failed;

  oldpool = framebuffersink->pool;
  framebuffersink->pool = newpool;

  g_mutex_unlock (&framebuffersink->flow_lock);

  if (oldpool) {
    gst_object_unref (oldpool);
  }

  g_sprintf(s, "Succesfully allocated buffer pool (alignment to %d byte boundary)",
    allocation_params.align + 1);
  GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink, s);

  if (!gst_buffer_pool_set_active(framebuffersink->pool, TRUE))
   goto activation_failed;

  GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink, "Succesfully activated buffer pool");

  return TRUE;

/* ERRORS */
config_failed:
  {
    GST_ERROR_OBJECT (framebuffersink, "Failed to set buffer pool config");
    g_mutex_unlock (&framebuffersink->flow_lock);
    return FALSE;
  }
activation_failed:
  {
    GST_ERROR_OBJECT(framebuffersink, "Activation of buffer pool failed");
    g_mutex_unlock (&framebuffersink->flow_lock);
    return FALSE;
  }
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
  GstFramebufferSinkOverlayType matched_overlay_type;
  int i;

  if (!gst_video_info_from_caps (&info, caps))
    goto invalid_format;

  if (gst_video_info_is_equal(&info, &framebuffersink->info))
    return TRUE;

  if (!framebuffersink->silent)
    g_print ("Negotiated caps: %" GST_PTR_FORMAT "\n", caps);

  /* Set the video parameters. */
  framebuffersink->fps_n = info.fps_n;
  framebuffersink->fps_d = info.fps_d;

  framebuffersink->videosink.width = info.width;
  framebuffersink->videosink.height = info.height;

  framebuffersink->video_width_in_bytes = framebuffersink->videosink.width *
      framebuffersink->bytespp;

  if (framebuffersink->video_width_in_bytes > framebuffersink->fixinfo.line_length)
    framebuffersink->video_width_in_bytes = framebuffersink->fixinfo.line_length;

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

  matched_overlay_type = gst_framebuffersink_video_format_to_overlay_type (GST_VIDEO_INFO_FORMAT (&info));

  if (framebuffersink->adjusted_dimensions) {
    framebuffersink->scaled_width = framebuffersink->adjusted_width;
    framebuffersink->scaled_height = framebuffersink->adjusted_height;
  }
  else {
    framebuffersink->scaled_width = info.width;
    framebuffersink->scaled_height = info.height;
    /* When using the hardware scaler, and upstream didn't call get_caps with */
    /* the negotiated caps, update the output dimensions for the scaler. */
    if (matched_overlay_type != GST_FRAMEBUFFERSINK_OVERLAY_TYPE_NONE) {
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
      || matched_overlay_type != GST_FRAMEBUFFERSINK_OVERLAY_TYPE_NONE)
      && framebuffersink->use_hardware_overlay) {
    /* The video dimensions are different from the requested ones, or the video format is YUV and we */
    /* are allowed to use the hardware overlay. */
    /* Calculate how may overlays fit in the available video memory (after the visible */
    /* screen. */
    int max_overlays = ((framebuffersink->max_framebuffers - 1)
        * framebuffersink->fixinfo.line_length * framebuffersink->varinfo.yres) / info.size;
    /* Limit the number of overlays used. */
    if (max_overlays > 30)
      max_overlays = 30;
    if (framebuffersink->hardware_overlay_types_supported[matched_overlay_type] && max_overlays >= 2 &&
        klass->prepare_overlay (framebuffersink, matched_overlay_type)) {
      /* Use the hardware overlay. */
      framebuffersink->nu_framebuffers_used = framebuffersink->max_framebuffers;
      framebuffersink->nu_overlays_used = max_overlays;
      if (framebuffersink->use_buffer_pool) {
        if (gst_framebuffersink_allocate_buffer_pool (framebuffersink, caps, &info)) {
          /* Use buffer pool. */
          if (!framebuffersink->silent)
            GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink,
                "Using custom buffer pool (streaming directly to video memory)");
          goto success_overlay;
        }
        framebuffersink->use_buffer_pool = FALSE;
        if (!framebuffersink->silent)
          GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, "Falling back to non buffer-pool mode");
      }
      /* Not using buffer pool. */
      if (framebuffersink->nu_overlays_used > 3)
        framebuffersink->nu_overlays_used = 3;
      goto success_overlay;
    }
  }

  framebuffersink->use_hardware_overlay = FALSE;

  if (matched_overlay_type == GST_FRAMEBUFFERSINK_OVERLAY_TYPE_I420)
    goto yuv_failed;

reconfigure:

  /* When using buffer pools, do the appropriate checks and allocate a */
  /* new buffer pool. */
  if (framebuffersink->use_buffer_pool) {
    if (framebuffersink->video_width_in_bytes != framebuffersink->fixinfo.line_length) {
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
    /* by default. */
    if (framebuffersink->use_buffer_pool) {
      if (framebuffersink->flip_buffers == 0 && framebuffersink->nu_framebuffers_used > 10)
        framebuffersink->nu_framebuffers_used = 10;
    }
    else
      /* When not using a buffer pool, only a few buffers are required for page flipping. */
      if (framebuffersink->flip_buffers == 0 && framebuffersink->nu_framebuffers_used > 3)
        framebuffersink->nu_framebuffers_used = 3;
    if (!framebuffersink->silent)
      g_print ("Using %d framebuffers for page flipping.\n", framebuffersink->nu_framebuffers_used);
  }
  if (framebuffersink->use_buffer_pool) {
    if (gst_framebuffersink_allocate_buffer_pool (framebuffersink, caps, &info)) {
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

  framebuffersink->info = info;

  /* Clear all used framebuffers to black. */
  if (framebuffersink->clear) {
    if (framebuffersink->use_hardware_overlay)
      gst_framebuffersink_clear_screen (framebuffersink, 0);
    else
      for (i = 0; i < framebuffersink->nu_framebuffers_used; i++)
        gst_framebuffersink_clear_screen (framebuffersink, i);
  }
  return TRUE;

success_overlay:

  if (!framebuffersink->silent) {
    char s[128];
    sprintf(s, "Using one framebuffer plus %d overlays in video memory.\n",
        framebuffersink->nu_overlays_used);
    GST_FRAMEBUFFERSINK_INFO_OBJECT (framebuffersink, s);
  }
  goto success;

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
yuv_failed:
  {
    GST_ERROR_OBJECT (framebuffersink,
        "Cannot not handle YUV format (hardware overlay failed)");
    return FALSE;
  }
}

/* The stop function should release resources. */

static gboolean
gst_framebuffersink_stop (GstBaseSink * sink)
{
  GstFramebufferSink *framebuffersink = GST_FRAMEBUFFERSINK (sink);
  GstFramebufferSinkClass *klass = GST_FRAMEBUFFERSINK_GET_CLASS (framebuffersink);

  GST_DEBUG_OBJECT (framebuffersink, "stop");

  if (framebuffersink->open_hardware_success)
    klass->close_hardware (framebuffersink);

  g_mutex_lock (&framebuffersink->flow_lock);

  free (framebuffersink->buffer_allocation_table);

  gst_framebuffersink_pan_display(framebuffersink, 0, 0);

  if (munmap (framebuffersink->framebuffer, framebuffersink->fixinfo.smem_len))
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

/* There are two different show frame functions, one copying frames from memory */
/* to video memory, and one that just pans to the frame that has already been */
/* streamed into video memory. */

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

  return GST_FLOW_OK;
}

/* This show frame function can deal with both video memory buffers */
/* that require a pan and with regular buffers that need to be memcpy-ed. */

static GstFlowReturn
gst_framebuffersink_show_frame_buffer_pool (GstFramebufferSink * framebuffersink,
GstBuffer * buf)
{
  GstMemory *mem;

  mem = gst_buffer_peek_memory (buf, 0);
  if (!mem)
    goto invalid_memory;

  if (GST_MEMORY_FLAG_IS_SET (mem, GST_MEMORY_FLAG_FRAMEBUFFERSINK)) {
    /* This a video memory buffer. */
    uint8_t *framebuffer_address;

    framebuffer_address = gst_mini_object_get_qdata ( GST_MINI_OBJECT (mem),
        framebuffersink->framebuffer_address_quark);
#if 0
    {
    char s[80];
    g_sprintf(s, "Video memory buffer encountered (0x%08X)", (guintptr) framebuffer_address);
    GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink, s);
    }
#endif

    gst_framebuffersink_put_image_pan(framebuffersink, framebuffer_address);

    return GST_FLOW_OK;
  } else {
    /* This is a normal memory buffer (system memory). */
#if 0
    GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink, "Non-video memory buffer encountered");
#endif

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

  mem = gst_buffer_peek_memory (buf, 0);
  if (!mem)
    goto invalid_memory;

  if (GST_MEMORY_FLAG_IS_SET (mem, GST_MEMORY_FLAG_FRAMEBUFFERSINK)) {
    /* This a video memory buffer. */
    uint8_t *framebuffer_address;

    framebuffer_address = gst_mini_object_get_qdata ( GST_MINI_OBJECT (mem),
        framebuffersink->framebuffer_address_quark);
#if 0
    {
    char s[80];
    g_sprintf(s, "Video memory buffer encountered (0x%08X)", (guintptr) framebuffer_address);
    GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink, s);
    }
#endif

    /* Wait for vsync before changing the overlay address. */
    if (framebuffersink->vsync)
      gst_framebuffersink_wait_for_vsync(framebuffersink);
    klass->show_overlay(framebuffersink, framebuffer_address - framebuffersink->framebuffer);

    return GST_FLOW_OK;
  } else {
    /* This is a normal memory buffer (system memory), but it is */
    /* overlay data. */
    GstMapInfo mapinfo = GST_MAP_INFO_INIT;

#if 0
    GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink, "Non-video memory buffer encountered");
#endif

    mem = gst_buffer_get_memory(buf, 0);
    gst_memory_map(mem, &mapinfo, GST_MAP_READ);

    /* Copy the image into video memory after the first screen. */
    gst_framebuffersink_put_overlay_image_memcpy(framebuffersink, mapinfo.data);
    framebuffersink->current_overlay_index++;
    if (framebuffersink->current_overlay_index >= framebuffersink->nu_overlays_used)
      framebuffersink->current_overlay_index = 0;

    gst_memory_unmap(mem, &mapinfo);
    gst_memory_unref(mem);
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
  if (framebuffersink->use_hardware_overlay)
    return gst_framebuffersink_show_frame_overlay(framebuffersink, buf);
  else if (framebuffersink->use_buffer_pool)
    return gst_framebuffersink_show_frame_buffer_pool(framebuffersink, buf);
  else
    return gst_framebuffersink_show_frame_memcpy(framebuffersink, buf);
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
  guint size;
  gboolean need_pool;

  gst_query_parse_allocation (query, &caps, &need_pool);

  if (caps == NULL)
    goto no_caps;

  /* Take a look at our pre-initialized pool in video memory. */
  g_mutex_lock (&framebuffersink->flow_lock);
  if ((pool = framebuffersink->pool))
    gst_object_ref (pool);
  g_mutex_unlock (&framebuffersink->flow_lock);

  /* If we had a buffer pool in video memory and it has been allocated, */
  /* we can't just provide regular system memory buffers because */
  /* they are currently mutually exclusive with video memory */
  /* buffers due to the difficulty of handling page flips correctly. */
  if (framebuffersink->use_buffer_pool && pool == NULL)
    return FALSE;

  if (pool != NULL) {
    GstCaps *pcaps;
    guintptr start_offset;

    /* We have a pool, check the caps. */
    GST_DEBUG_OBJECT (framebuffersink, "check existing pool caps");
    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_get_params (config, &pcaps, &size, NULL, NULL);

    /* Also check the start offset (different for hardware overlay mode). */
    start_offset = 0;
    if (framebuffersink->use_hardware_overlay)
      start_offset = framebuffersink->fixinfo.line_length * framebuffersink->varinfo.yres;
    if (start_offset != framebuffersink->allocation_start_offset ||
    !gst_caps_is_equal (caps, pcaps)) {
      GST_DEBUG_OBJECT (framebuffersink, "pool has different caps");
      /* Different caps, we can't use our pool. */
      gst_object_unref (pool);
      pool = NULL;
    }
    gst_structure_free (config);
  }

  /* At this point if pool is not NULL our current allocated */
  /* pool is a match. */
  if (pool != NULL) {
    int n = framebuffersink->nu_framebuffers_used;
    if (framebuffersink->use_hardware_overlay)
      n = framebuffersink->nu_overlays_used;
    gst_query_add_allocation_pool (query, pool, size, n, n);
    g_object_unref(pool);
    g_mutex_lock (&framebuffersink->flow_lock);
    framebuffersink->pool = NULL;
    g_mutex_unlock (&framebuffersink->flow_lock);

#if 0
    GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink,
      "propose_allocation: provide our video memory buffer pool");
#endif
    goto end;
  }

  if (pool == NULL && need_pool) {
    GstVideoInfo info;

    /* If not, and upstream requires a pool, we can't provide */
    /* a suitable pool from video memory, so provide a default one. */

    if (!gst_video_info_from_caps (&info, caps))
      goto invalid_caps;

    GST_DEBUG_OBJECT (framebuffersink, "create new pool");
    pool = gst_buffer_pool_new ();

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
    GST_DEBUG_OBJECT (bsink, "no caps specified");
    return FALSE;
  }
invalid_caps:
  {
    GST_DEBUG_OBJECT (bsink, "invalid caps specified");
    return FALSE;
  }
config_failed:
  {
    GST_DEBUG_OBJECT (bsink, "failed setting config");
    gst_object_unref (pool);
    return FALSE;
  }
}

/*
 * Memory allocator implementation;
 * Create our own simple allocator for video memory buffers.
 */

static gboolean
gst_framebuffersink_buffer_allocation_table_configure (GstFramebufferSink *framebuffersink,
gsize size, guintptr start_offset) {
  int i, n;

  g_mutex_lock(&framebuffersink->flow_lock);

  if (framebuffersink->buffer_allocation_table == NULL)
    goto configure;

  if (size == framebuffersink->allocation_size && start_offset ==
  framebuffersink->allocation_start_offset)
    goto success;

  /* If the allocation table is empty, we can reconfigure it for the new size. */
  for (i = 0; i < framebuffersink->buffer_allocation_table_size; i++)
    if (framebuffersink->buffer_allocation_table[i] == 1)
      break;

  if (i == framebuffersink->buffer_allocation_table_size)
    goto configure;

  /* Not empty, so we can't reconfigure it. */
  g_mutex_unlock(&framebuffersink->flow_lock);
  return FALSE;

configure:
    /* Configure. */
  n = (framebuffersink->fixinfo.smem_len - start_offset) / size;
  framebuffersink->buffer_allocation_table_size = n;
  framebuffersink->allocation_size = size;
  framebuffersink->allocation_start_offset = start_offset;
  free(framebuffersink->buffer_allocation_table);
  framebuffersink->buffer_allocation_table = malloc (sizeof (uint8_t) * n);
  memset (framebuffersink->buffer_allocation_table, 0, sizeof (uint8_t) * n);

success:
  g_mutex_unlock(&framebuffersink->flow_lock);

  return TRUE;
}

static int
gst_framebuffersink_alloc_framebuffer_slot (GstFramebufferSink *framebuffersink) {
  int i;
  for (i = 0; i < framebuffersink->buffer_allocation_table_size; i++)
    if (framebuffersink->buffer_allocation_table[i] == 0) {
      framebuffersink->buffer_allocation_table[i] = 1;
      return i;
    }
  return - 1;
}

static void
gst_framebuffersink_free_framebuffer_slot (GstFramebufferSink *framebuffersink, int i) {
  if (framebuffersink->buffer_allocation_table[i] == 1) {
      framebuffersink->buffer_allocation_table[i] = 0;
      return;
  }
  GST_ERROR_OBJECT(framebuffersink, "Framebuffer slot is already unused");
}

#define gst_framebuffersink_allocator_parent_class allocator_parent_class
G_DEFINE_TYPE (GstFramebufferSinkAllocator, gst_framebuffersink_allocator, GST_TYPE_ALLOCATOR);

static GstFramebufferSinkAllocator *gst_framebuffersink_allocator_new (
GstFramebufferSink * framebuffersink) {
  int i;
  GstFramebufferSinkAllocator *framebuffersink_allocator;
  framebuffersink_allocator = g_object_new (GST_TYPE_FRAMEBUFFERSINK_ALLOCATOR, NULL);
  framebuffersink_allocator->framebuffersink = framebuffersink;
  framebuffersink_allocator->buffers = malloc(framebuffersink->buffer_allocation_table_size * sizeof(int));
  for (i = 0; i < framebuffersink->buffer_allocation_table_size; i++)
    framebuffersink_allocator->buffers[i] = - 1;
  return framebuffersink_allocator;
}

static GstMemory *
gst_framebuffersink_allocator_alloc (GstAllocator * allocator, gsize size,
GstAllocationParams *params) {
  GstFramebufferSinkAllocator * framebuffersink_allocator =
      GST_FRAMEBUFFERSINK_ALLOCATOR (allocator);
  GstFramebufferSink * framebuffersink = framebuffersink_allocator->framebuffersink;
  guintptr offset;
  uint8_t * new_framebuffer;
  int i;

  GST_DEBUG_OBJECT(framebuffersink, "allocator_alloc");

  g_mutex_lock(&framebuffersink->flow_lock);

  i =  gst_framebuffersink_alloc_framebuffer_slot(framebuffersink);
  offset = i * size + framebuffersink->allocation_start_offset;
  if (i < 0) {
    GST_ERROR_OBJECT(framebuffersink_allocator,
        "Not enough framebuffer memory for requested buffer (shouldn't happen)");
        new_framebuffer = framebuffersink->framebuffer;
  }
  else {
    int j;
    new_framebuffer = framebuffersink->framebuffer + offset;
    /* Add the buffer number to the allocator's allocation table. */
    for (j = 0; j < framebuffersink->buffer_allocation_table_size; j++)
      if (framebuffersink_allocator->buffers[j] < 0) {
        framebuffersink_allocator->buffers[j] = i;
        break;
      }
   if (j == framebuffersink->buffer_allocation_table_size) {
     GST_ERROR_OBJECT(framebuffersink_allocator,
       "Couldn't find free slot in allocator's allocation table (shouldn't happen)");
   }
  }

  g_mutex_unlock(&framebuffersink->flow_lock);

#if 0
  g_sprintf(s, "Allocated framebuffer #%d at 0x%08X", i, (uint32_t)new_framebuffer);
  GST_FRAMEBUFFERSINK_INFO_OBJECT(framebuffersink, s);
#endif

  GstMemory *mem = gst_memory_new_wrapped(
      0, /* GST_MEMORY_PHYSICALLY_CONTIGUOUS */ /* Needs Gstreamer 1.2. */
      new_framebuffer, size, 0, size, NULL, NULL);
  /* Set flag to mark the memory as our own. */
  GST_MINI_OBJECT_FLAG_SET(mem, GST_MEMORY_FLAG_FRAMEBUFFERSINK);
  /* Add user data to store the original mapped framebuffer */
  /* address. */
  gst_mini_object_set_qdata (GST_MINI_OBJECT (mem), framebuffersink->framebuffer_address_quark,
      new_framebuffer, NULL);
  return mem;
}

static void
gst_framebuffersink_allocator_free(GstAllocator * allocator, GstMemory *memory) {
  GstFramebufferSinkAllocator *framebuffersink_allocator =
      GST_FRAMEBUFFERSINK_ALLOCATOR(allocator);
  GstFramebufferSink * framebuffersink = framebuffersink_allocator->framebuffersink;
  GstMapInfo mapinfo;
  int i, j;

  GST_DEBUG_OBJECT(framebuffersink_allocator, "allocator_free");

  /* Calculate the framebuffer index number from the address. */
  i = ( (uint8_t *) gst_mini_object_get_qdata ( GST_MINI_OBJECT (memory),
      framebuffersink->framebuffer_address_quark) - (framebuffersink->framebuffer
      + framebuffersink->allocation_start_offset)) / mapinfo.size;

  g_mutex_lock(&framebuffersink->flow_lock);

  /* Find the slot in the buffer table and clear it. */
  for (j = 0; j < framebuffersink->buffer_allocation_table_size; j++)
    if (framebuffersink_allocator->buffers[j] == i)
        framebuffersink_allocator->buffers[j] = - 1;

  gst_framebuffersink_free_framebuffer_slot(framebuffersink, i);

  g_mutex_unlock(&framebuffersink->flow_lock);

  g_slice_free (GstMemory, memory);
}

static void
gst_framebuffersink_allocator_finalize(GObject *object) {
}

static void
gst_framebuffersink_allocator_class_init (GstFramebufferSinkAllocatorClass * klass) {
  GObjectClass * g_object_class = G_OBJECT_CLASS (klass);
  GstAllocatorClass * allocator_class = GST_ALLOCATOR_CLASS (klass);

  g_object_class->finalize = gst_framebuffersink_allocator_finalize;
  allocator_class->alloc = gst_framebuffersink_allocator_alloc;
  allocator_class->free = gst_framebuffersink_allocator_free;
}

static void
gst_framebuffersink_allocator_init (GstFramebufferSinkAllocator *framebuffersink_allocator) {
  GstAllocator * allocator = GST_ALLOCATOR (framebuffersink_allocator);

  GST_OBJECT_FLAG_SET (allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);

  gst_allocator_register("framebuffersink_allocator", allocator);
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
