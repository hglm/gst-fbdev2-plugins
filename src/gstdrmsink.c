/* GStreamer drmsink plugin
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
/*
 * Based partly on gstkmssink.c found at https://gitorious.org/vjaquez-gstreamer/
 * which has the following copyright message.
 *
 * Copyright (C) 2012 Texas Instruments
 * Copyright (C) 2012 Collabora Ltd
 *
 * Authors:
 *  Alessandro Decina <alessandro.decina@collabora.co.uk>
 *  Víctor Manuel Jáquez Leal <vjaquez@igalia.com>
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-drmsink
 *
 * The drmsink element implements an accelerated and optimized
 * video sink for the Linux console framebuffer using the libdrm library.
 * The basis of the implementation is the optimized framebuffer sink as
 * implemented in the GstFramebufferSink class.
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
 * gst-launch -v videotestsrc ! drmsink >/dev/null
 * ]|
 * Output the video test signal to the framebuffer. The redirect to
 * null surpressed interference from console text mode.
 * |[
 * gst-launch -v videotestsrc ! drmsink native-resolution=true
 * ]|
 * Run videotstsrc at native screen resolution
 * |[
 * gst-launch -v videotestsrc horizontal_speed=10 ! drmsink \
 * native-resolution=true buffer-pool=true graphics-mode=true
 * ]|
 * This command illustrates some of the plugin's optimization features
 * by rendering to video memory with vsync and page flipping in
 * console graphics mode. There should be no tearing with page flipping/
 * vsync enabled. You might have to use the fps property to reduce the frame
 * rate on slower systems.
 * |[
 * gst-launch playbin uri=[uri] video-sink="drmsink native-resolution=true"
 * ]|
 * Use playbin while passing options to drmsink.
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
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>
#include <glib/gprintf.h>
#include <xf86drmMode.h>
#include <xf86drm.h>
#include <libkms.h>
#include <gbm.h>

#include <gst/gst.h>
#include <gst/video/gstvideosink.h>
#include <gst/video/video.h>
#include <gst/video/video-info.h>
#include "gstdrmsink.h"

// #define USE_DRM_PLANES

GST_DEBUG_CATEGORY_STATIC (gst_drmsink_debug_category);
#define GST_CAT_DEFAULT gst_drmsink_debug_category

/* Inline function to produce both normal message and debug info. */
static inline void GST_DRMSINK_INFO_OBJECT (GstDrmsink *drmsink,
const gchar *message) {
  if (!drmsink->framebuffersink.silent) g_print ("%s.\n", message);
  GST_INFO_OBJECT (drmsink, message);
}

#define DEFAULT_PROP_DRMDEVICE "/dev/dri/card0"

/* Class function prototypes. */
static void gst_drmsink_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_drmsink_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);

static gboolean gst_drmsink_open_hardware (GstFramebufferSink *framebuffersink,
    GstVideoInfo *info);
static void gst_drmsink_close_hardware (GstFramebufferSink *framebuffersink);
static GstAllocator *gst_drmsink_video_memory_allocator_new (GstFramebufferSink *
    framebuffersink, GstVideoInfo *info, gboolean pannable, gboolean is_overlay);
static void gst_drmsink_pan_display (GstFramebufferSink *framebuffersink,
    GstMemory *memory);
static void gst_drmsink_wait_for_vsync (GstFramebufferSink *framebuffersink);

/* Local functions. */
static void gst_drmsink_reset (GstDrmsink *drmsink);
static void gst_drmsink_vblank_handler (int fd, unsigned int sequence, unsigned int tv_sec,
    unsigned int tv_usec, void *user_data);
static void gst_drmsink_page_flip_handler (int fd,  unsigned int sequence,
    unsigned int tv_sec, unsigned int tv_usec, void *user_data);

enum
{
  PROP_0,
  PROP_CONNECTOR,
  PROP_DRMDEVICE,
};

#define GST_DRMSINK_TEMPLATE_CAPS \
        GST_VIDEO_CAPS_MAKE ("RGB") \
        "; " GST_VIDEO_CAPS_MAKE ("BGR") \
        "; " GST_VIDEO_CAPS_MAKE ("RGBx") \
        "; " GST_VIDEO_CAPS_MAKE ("BGRx") \
        "; " GST_VIDEO_CAPS_MAKE ("xRGB") \
        "; " GST_VIDEO_CAPS_MAKE ("xBGR") ", " \
        "framerate = (fraction) [ 0, MAX ], " \
        "width = (int) [ 1, MAX ], " "height = (int) [ 1, MAX ]"


static GstStaticPadTemplate gst_drmsink_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_DRMSINK_TEMPLATE_CAPS)
    );

/* Class initialization. */

#define gst_drmsink_parent_class videosink_parent_class
G_DEFINE_TYPE_WITH_CODE (GstDrmsink, gst_drmsink, GST_TYPE_FRAMEBUFFERSINK,
  GST_DEBUG_CATEGORY_INIT (gst_drmsink_debug_category, "drmsink", 0,
  "debug category for drmsink element"));

static void
gst_drmsink_class_init (GstDrmsinkClass* klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstFramebufferSinkClass *framebuffer_sink_class = GST_FRAMEBUFFERSINK_CLASS (klass);

  gobject_class->set_property = gst_drmsink_set_property;
  gobject_class->get_property = gst_drmsink_get_property;

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS(klass),
      gst_static_pad_template_get (&gst_drmsink_sink_template));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS(klass),
      "Optimized Linux console libdrm/KMS sink",
      "Sink/Video",
      "drm framebuffer sink",
      "Harm Hanemaaijer <fgenfb@yahoo.com>");

  g_object_class_install_property (gobject_class, PROP_CONNECTOR,
      g_param_spec_int ("connector", "Connector", "DRM connector id",
      0, G_MAXINT32, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_DRMDEVICE,
      g_param_spec_string ("drm-device", "DRM device", "DRM device",
      DEFAULT_PROP_DRMDEVICE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  framebuffer_sink_class->open_hardware = GST_DEBUG_FUNCPTR (gst_drmsink_open_hardware);
  framebuffer_sink_class->close_hardware = GST_DEBUG_FUNCPTR (gst_drmsink_close_hardware);
  framebuffer_sink_class->wait_for_vsync = GST_DEBUG_FUNCPTR (gst_drmsink_wait_for_vsync);
  framebuffer_sink_class->pan_display = GST_DEBUG_FUNCPTR (gst_drmsink_pan_display);
  framebuffer_sink_class->video_memory_allocator_new = GST_DEBUG_FUNCPTR (
      gst_drmsink_video_memory_allocator_new);
}

/* Class member functions. */

static void
gst_drmsink_init (GstDrmsink *drmsink) {

  /* Set the initial values of the properties.*/
  drmsink->devicefile = g_strdup (DEFAULT_PROP_DRMDEVICE);
  drmsink->preferred_connector_id = - 1;
  drmsink->fd = -1;
  gst_drmsink_reset (drmsink);
}

void
gst_drmsink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstDrmsink *drmsink = GST_DRMSINK (object);

  GST_DEBUG_OBJECT (drmsink, "set_property");
  g_return_if_fail (GST_IS_DRMSINK (object));

  switch (prop_id) {
    case PROP_CONNECTOR:
      drmsink->preferred_connector_id = g_value_get_int (value);
      break;
    case PROP_DRMDEVICE:
      drmsink->devicefile = g_strdup (g_value_get_string (value));
      break;
    default:
      break;
    }
}

static void
gst_drmsink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstDrmsink *drmsink = GST_DRMSINK (object);

  GST_DEBUG_OBJECT (drmsink, "get_property");
  g_return_if_fail (GST_IS_DRMSINK (object));

  switch (prop_id) {
    case PROP_CONNECTOR:
      g_value_set_int (value, drmsink->preferred_connector_id);
      break;
    case PROP_DRMDEVICE:
      g_value_set_string (value, drmsink->devicefile);
      break;
    default:
      break;
    }
}

static gboolean
gst_drmsink_find_mode_and_plane (GstDrmsink *drmsink, GstVideoRectangle *dim)
{
  drmModeConnector *connector;
  drmModeEncoder *encoder;
  drmModeModeInfo *mode;
  drmModePlane *plane;
  int i, pipe;
  gboolean ret;
  char s[80];

  ret = FALSE;
  encoder = NULL;

  /* First, find the connector & mode */
  connector = drmModeGetConnector (drmsink->fd, drmsink->connector_id);
  if (!connector)
    goto error_no_connector;

  if (connector->count_modes == 0)
    goto error_no_mode;

  g_sprintf(s, "Connected encoder: id = %u", connector->encoder_id);
  GST_DRMSINK_INFO_OBJECT (drmsink, s);
  for (i = 0; i < connector->count_encoders; i++) {
    g_sprintf(s, "Available encoder: id = %u", connector->encoders[i]);
    GST_DRMSINK_INFO_OBJECT (drmsink, s);
  }

  /* Now get the encoder */
  encoder = drmModeGetEncoder (drmsink->fd, connector->encoder_id);
  if (!encoder)
    goto error_no_encoder;

  /* XXX: just pick the first available mode, which has the highest
   * resolution. */
  mode = &connector->modes[0];
  memcpy (&drmsink->mode, &connector->modes[0], sizeof (connector->modes[0]));

  dim->x = dim->y = 0;
  dim->w = mode->hdisplay;
  dim->h = mode->vdisplay;
  GST_INFO_OBJECT (drmsink, "connector mode = %dx%d", dim->w, dim->h);

  drmsink->crtc_id = encoder->crtc_id;

  /* and figure out which crtc index it is: */
  pipe = -1;
  for (i = 0; i < drmsink->resources->count_crtcs; i++) {
    if (drmsink->crtc_id == drmsink->resources->crtcs[i]) {
      pipe = i;
      break;
    }
  }

  if (pipe == -1)
    goto error_no_crtc;

#ifdef USE_DRM_PLANES
  for (i = 0; i < drmsink->plane_resources->count_planes; i++) {
    plane = drmModeGetPlane (drmsink->fd, drmsink->plane_resources->planes[i]);
    if (plane->possible_crtcs & (1 << pipe)) {
      drmsink->plane = plane;
      break;
    } else {
      drmModeFreePlane (plane);
    }
  }

  if (!drmsink->plane)
    goto error_no_plane;
#endif

  ret = TRUE;

fail:
  if (encoder)
    drmModeFreeEncoder (encoder);

  if (connector)
    drmModeFreeConnector (connector);

  return ret;

error_no_connector:
  GST_ERROR_OBJECT (drmsink, "could not get connector (%d): %s",
      drmsink->connector_id, strerror (errno));
  goto fail;

error_no_mode:
  GST_ERROR_OBJECT (drmsink, "could not find a valid mode (count_modes %d)",
      connector->count_modes);
  goto fail;

error_no_encoder:
  GST_ERROR_OBJECT (drmsink, "could not get encoder: %s", strerror (errno));
  goto fail;

error_no_crtc:
  GST_ERROR_OBJECT (drmsink, "couldn't find a crtc");
  goto fail;

error_no_plane:
  GST_ERROR_OBJECT (drmsink, "couldn't find a plane");
  goto fail;
}

static void
gst_drmsink_reset (GstDrmsink *drmsink)
{

#ifdef USE_DRM_PLANES
  if (drmsink->plane) {
    drmModeFreePlane (drmsink->plane);
    drmsink->plane = NULL;
  }

  if (drmsink->plane_resources) {
    drmModeFreePlaneResources (drmsink->plane_resources);
    drmsink->plane_resources = NULL;
  }
#endif

  if (drmsink->resources) {
    drmModeFreeResources (drmsink->resources);
    drmsink->resources = NULL;
  }


  if (drmsink->fd != -1) {
    close (drmsink->fd);
    drmsink->fd = -1;
  }

//  drmsink->par_n = drmsink->par_d = 1;

  memset (&drmsink->screen_rect, 0, sizeof (GstVideoRectangle));
//  memset (&drmsink->info, 0, sizeof (GstVideoInfo));

  drmsink->connector_id = -1;
}

static gboolean
gst_drmsink_open_hardware (GstFramebufferSink *framebuffersink, GstVideoInfo *info) {
  GstDrmsink *drmsink = GST_DRMSINK (framebuffersink);

  drmModeConnector *connector = NULL;
  int i;
  int res;
  uint64_t has_dumb_buffers;
  gsize size;
  gchar *s;

  if (!drmAvailable()) {
    GST_DRMSINK_INFO_OBJECT (drmsink, "No kernel DRM driver loaded");
    return FALSE;
  }

  /* Open drm device. */
  drmsink->fd = open (drmsink->devicefile, O_RDWR | O_CLOEXEC);
  if (drmsink->fd < 0) {
    s = g_strdup_printf ("Cannot open DRM device %s", drmsink->devicefile);
    GST_DRMSINK_INFO_OBJECT (drmsink, s);
    g_free (s);
    return FALSE;
  }

  res = drmGetCap (drmsink->fd, DRM_CAP_DUMB_BUFFER, &has_dumb_buffers);
  if (res < 0 || !has_dumb_buffers) {
    GST_DRMSINK_INFO_OBJECT (drmsink, "DRM device does not support dumb buffers");
    return FALSE;
  }

  drmsink->resources = drmModeGetResources (drmsink->fd);
  if (drmsink->resources == NULL)
    goto resources_failed;

  for (i = 0; i < drmsink->resources->count_connectors; i++) {
    int j;
    connector = drmModeGetConnector(drmsink->fd, drmsink->resources->connectors[i]);

    s = g_strdup_printf ("DRM connector found, id = %d, type = %d, connected = %d",
        connector->connector_id, connector->connector_type,
        connector->connection == DRM_MODE_CONNECTED);
    GST_DRMSINK_INFO_OBJECT (drmsink, s);
    g_free (s);
    for (j = 0; j < connector->count_modes; j++) {
      s = g_strdup_printf ("Supported mode %s", connector->modes[j].name);
      GST_DRMSINK_INFO_OBJECT (drmsink, s);
      g_free (s);
    }
    drmModeFreeConnector(connector);
  }

  if (drmsink->preferred_connector_id >= 0) {
    /* Connector specified as property. */
    for (i = 0; i < drmsink->resources->count_connectors; i++) {
      connector = drmModeGetConnector(drmsink->fd, drmsink->resources->connectors[i]);
      if (!connector)
        continue;

      if (connector->connector_id == drmsink->preferred_connector_id)
        break;

      drmModeFreeConnector(connector);
    }

    if (i == drmsink->resources->count_connectors) {
      GST_DRMSINK_INFO_OBJECT (drmsink, "Specified DRM connector not found");
      drmModeFreeResources (drmsink->resources);
      return FALSE;
    }

    drmsink->connector_id = drmsink->preferred_connector_id;
  }
  else {
    /* Look for active connectors. */
    for (i = 0; i < drmsink->resources->count_connectors; i++) {
      connector = drmModeGetConnector(drmsink->fd, drmsink->resources->connectors[i]);
      if (!connector)
        continue;

      if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0)
        break;

      drmModeFreeConnector(connector);
    }

    if (i == drmsink->resources->count_connectors) {
      GST_DRMSINK_INFO_OBJECT (drmsink, "No currently active DRM connector found");
      drmModeFreeResources (drmsink->resources);
      return FALSE;
    }

    drmsink->connector_id = connector->connector_id;
  }

#ifdef USE_DRM_PLANES
  drmsink->plane_resources = drmModeGetPlaneResources (drmsink->fd);
  if (drmsink->plane_resources == NULL)
    goto plane_resources_failed;
#endif

  gst_drmsink_find_mode_and_plane (drmsink, &drmsink->screen_rect);

  drmsink->crtc_mode_initialized = FALSE;
  drmsink->saved_crtc = drmModeGetCrtc (drmsink->fd, drmsink->crtc_id);

  drmsink->event_context = g_slice_new (drmEventContext);
  drmsink->event_context->version = DRM_EVENT_CONTEXT_VERSION;
  drmsink->event_context->vblank_handler = gst_drmsink_vblank_handler;
  drmsink->event_context->page_flip_handler = gst_drmsink_page_flip_handler;

#if 0
  drmModeFreeResources(resources);

  /* Create libkms driver. */

  ret = kms_create(drmsink->fd, &drmsink->drv);
  if (ret) {
    GST_DRMSINK_INFO_OBJECT(drmsink, "kms_create() failed");
    return FALSE;
  }
#endif

  gst_video_info_set_format (info, GST_VIDEO_FORMAT_BGRx, drmsink->screen_rect.w,
      drmsink->screen_rect.h);
  size = GST_VIDEO_INFO_COMP_STRIDE (info, 0) * GST_VIDEO_INFO_HEIGHT (info);

  /* GstFramebufferSink expects the max number of allocatable screen buffers to
     be set. Because DRM doesn't really allow querying of available video memory,
     assume three buffers are available and rely on a specific setting of the
     video-memory property for more buffers. */
  framebuffersink->max_framebuffers = 3;
  if (framebuffersink->max_video_memory_property > 0) {
    framebuffersink->max_framebuffers = (guint64)framebuffersink->max_video_memory_property
        * 1024 * 1024 / size;
    if (framebuffersink->max_framebuffers < 1)
      framebuffersink->max_framebuffers = 1;
  }

  s = g_strdup_printf("Successfully initialized DRM, connector = %d, mode = %dx%d",
      drmsink->connector_id, drmsink->screen_rect.w, drmsink->screen_rect.h);
  GST_DRMSINK_INFO_OBJECT (drmsink, s);
  g_free (s);

  return TRUE;

fail:
  gst_drmsink_reset (drmsink);
  return FALSE;

resources_failed:
  GST_ELEMENT_ERROR (drmsink, RESOURCE, FAILED,
      (NULL), ("drmModeGetResources failed: %s (%d)", strerror (errno), errno));
  goto fail;

plane_resources_failed:
  GST_ELEMENT_ERROR (drmsink, RESOURCE, FAILED,
      (NULL), ("drmModeGetPlaneResources failed: %s (%d)",
          strerror (errno), errno));
  goto fail;
}

static void
gst_drmsink_close_hardware (GstFramebufferSink *framebuffersink) {
  GstDrmsink *drmsink = GST_DRMSINK (framebuffersink);

  drmModeSetCrtc (drmsink->fd, drmsink->saved_crtc->crtc_id,
      drmsink->saved_crtc->buffer_id, drmsink->saved_crtc->x,
      drmsink->saved_crtc->y, &drmsink->connector_id, 1,
      &drmsink->saved_crtc->mode);
  drmModeFreeCrtc (drmsink->saved_crtc);

  gst_drmsink_reset (drmsink);

  GST_DRMSINK_INFO_OBJECT (drmsink, "Closed DRM device");

  return;
}

static void
gst_drmsink_vblank_handler (int fd, unsigned int sequence, unsigned int tv_sec,
    unsigned int tv_usec, void *user_data)
{
}

static void
gst_drmsink_page_flip_handler (int fd,  unsigned int sequence,
    unsigned int tv_sec, unsigned int tv_usec, void *user_data)
{
    GstDrmsink *drmsink = (GstDrmsink *)user_data;
    drmsink->page_flip_occurred = TRUE;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  /* Remember to set the rank if it's an element that is meant
     to be autoplugged by decodebin. */
  return gst_element_register (plugin, "drmsink", GST_RANK_NONE,
      GST_TYPE_DRMSINK);
}

/* these are normally defined by the GStreamer build system.
   If you are creating an element to be included in gst-plugins-*,
   remove these, as they're always defined.  Otherwise, edit as
   appropriate for your external plugin package. */
#ifndef VERSION
#define VERSION "0.1"
#endif
#ifndef PACKAGE
#define PACKAGE "gstdrmsink"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "gstreamer1.0-fbdev2-plugins"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/hglm"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    drmsink,
    "Optimized Linux console libdrm/KMS sink",
    plugin_init, VERSION, "LGPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)


/* DRM video memory allocator. */

typedef struct
{
  GstAllocator parent;
  GstDrmsink *drmsink;
  int w;
  int h;
  GstVideoFormatInfo format_info;
  /* The amount of video memory allocated. */
  gsize total_allocated;
} GstDrmSinkVideoMemoryAllocator;

typedef struct
{
  GstAllocatorClass parent_class;
} GstDrmSinkVideoMemoryAllocatorClass;

GType gst_drmsink_video_memory_allocator_get_type (void);
G_DEFINE_TYPE (GstDrmSinkVideoMemoryAllocator, gst_drmsink_video_memory_allocator, GST_TYPE_ALLOCATOR);

typedef struct
{
  GstMemory mem;
  struct drm_mode_create_dumb creq;
  struct drm_mode_map_dumb mreq;
  uint32_t fb;
  gpointer map_address;
} GstDrmSinkVideoMemory;

static GstMemory *
gst_drmsink_video_memory_allocator_alloc (GstAllocator *allocator, gsize size,
GstAllocationParams *params)
{
  GstDrmSinkVideoMemoryAllocator *drmsink_video_memory_allocator =
      (GstDrmSinkVideoMemoryAllocator *)allocator;
  GstDrmSinkVideoMemory *mem;
  struct drm_mode_destroy_dumb dreq;
  int ret;
  /* Ignore params (which should be NULL) and use word alignment. */
  int align = 3;
  int i;
  int depth;

  GST_OBJECT_LOCK (allocator);

  mem = g_slice_new (GstDrmSinkVideoMemory);

  mem->creq.height = drmsink_video_memory_allocator->h;
  mem->creq.width = drmsink_video_memory_allocator->w;
  mem->creq.bpp = GST_VIDEO_FORMAT_INFO_PSTRIDE (&drmsink_video_memory_allocator->format_info, 0) * 8;
  mem->creq.flags = 0;

  /* handle, pitch and size will be returned in the creq struct. */
  ret = drmIoctl(drmsink_video_memory_allocator->drmsink->fd, DRM_IOCTL_MODE_CREATE_DUMB,
      &mem->creq);
  if (ret < 0) {
    g_print ("Creating dumb drm buffer failed.\n");
    g_slice_free (GstDrmSinkVideoMemory, mem);
    GST_OBJECT_UNLOCK (allocator);
    return NULL;
  }

  depth = 0;
  for (i = 0; i < GST_VIDEO_FORMAT_INFO_N_COMPONENTS (&drmsink_video_memory_allocator->format_info); i++)
    depth += GST_VIDEO_FORMAT_INFO_DEPTH (&drmsink_video_memory_allocator->format_info, i);

  /* create framebuffer object for the dumb-buffer */
  ret = drmModeAddFB(drmsink_video_memory_allocator->drmsink->fd,
      drmsink_video_memory_allocator->w, drmsink_video_memory_allocator->h, depth,
      GST_VIDEO_FORMAT_INFO_PSTRIDE (&drmsink_video_memory_allocator->format_info, 0) * 8,
      mem->creq.pitch, mem->creq.handle, &mem->fb);
  if (ret) {
    /* frame buffer creation failed; see "errno" */
    g_print ("DRM framebuffer creation failed.\n");
    goto fail_destroy;
  }

  /* the framebuffer "fb" can now used for scanout with KMS */

  /* prepare buffer for memory mapping */
  memset(&mem->mreq, 0, sizeof(mem->mreq));
  mem->mreq.handle = mem->creq.handle;
  ret = drmIoctl(drmsink_video_memory_allocator->drmsink->fd, DRM_IOCTL_MODE_MAP_DUMB,
      &mem->mreq);
  if (ret) {
    g_print ("DRM buffer preparation failed.\n");
    drmModeRmFB(drmsink_video_memory_allocator->drmsink->fd, mem->creq.handle);
    goto fail_destroy;
  }

  /* mem->mreq.offset now contains the new offset that can be used with mmap() */

  /* perform actual memory mapping */
  mem->map_address = mmap(0, mem->creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
      drmsink_video_memory_allocator->drmsink->fd, mem->mreq.offset);
  if (mem->map_address == MAP_FAILED) {
    /* memory-mapping failed; see "errno" */
    g_print ("Memory mapping of DRM buffer failed.\n");
    drmModeRmFB(drmsink_video_memory_allocator->drmsink->fd, mem->creq.handle);
    goto fail_destroy;
  }

  gst_memory_init (GST_MEMORY_CAST (mem), GST_MEMORY_FLAG_NO_SHARE |
      GST_MEMORY_FLAG_VIDEO_MEMORY,
      (GstAllocator *)drmsink_video_memory_allocator, NULL, size, align, 0, size);

  drmsink_video_memory_allocator->total_allocated += size;

  g_print ("Allocated video memory buffer of size %zd at %p, align %d, mem = %p\n", size,
      mem->map_address, align, mem);

  memset (mem->map_address, rand() | 0xFF000000, size);

  GST_OBJECT_UNLOCK (allocator);
  return (GstMemory *) mem;

fail_destroy :

    dreq.handle = mem->creq.handle;
    drmIoctl(drmsink_video_memory_allocator->drmsink->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    g_slice_free (GstDrmSinkVideoMemory, mem);
    GST_OBJECT_UNLOCK (allocator);
    return NULL;
}

static void
gst_drmsink_video_memory_allocator_free (GstAllocator * allocator, GstMemory * mem)
{
  GstDrmSinkVideoMemoryAllocator *drmsink_video_memory_allocator =
      (GstDrmSinkVideoMemoryAllocator *)allocator;
  GstDrmSinkVideoMemory *vmem = (GstDrmSinkVideoMemory *) mem;
  struct drm_mode_destroy_dumb dreq;

//  g_print("video_memory_allocator_free called, address = %p\n", vmem->data);

  drmsink_video_memory_allocator->total_allocated -= mem->size;

  munmap(vmem->map_address, vmem->creq.size);
  dreq.handle = vmem->creq.handle;
  drmIoctl(drmsink_video_memory_allocator->drmsink->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);

  g_slice_free1 (sizeof (GstDrmSinkVideoMemory), vmem);

  GST_DEBUG ("%p: freed", vmem);
}

static gpointer
gst_drmsink_video_memory_map (GstDrmSinkVideoMemory * mem, gsize maxsize, GstMapFlags flags)
{
//  g_print ("video_memory_map called, mem = %p, maxsize = %d, flags = %d, data = %p\n", mem,
//      maxsize, flags, mem->map_address);

//  if (flags & GST_MAP_READ)
//    g_print ("Mapping video memory for reading is slow.\n");

  return mem->map_address;
}

static gboolean
gst_drmsink_video_memory_unmap (GstDrmSinkVideoMemory * mem)
{
  GST_DEBUG ("%p: unmapped", mem);
  return TRUE;
}


static void
gst_drmsink_video_memory_allocator_class_init (GstDrmSinkVideoMemoryAllocatorClass * klass) {
  GstAllocatorClass * allocator_class = GST_ALLOCATOR_CLASS (klass);

  allocator_class->alloc = gst_drmsink_video_memory_allocator_alloc;
  allocator_class->free = gst_drmsink_video_memory_allocator_free;
}

static void
gst_drmsink_video_memory_allocator_init (GstDrmSinkVideoMemoryAllocator *video_memory_allocator) {
  GstAllocator * alloc = GST_ALLOCATOR_CAST (video_memory_allocator);

  alloc->mem_type = "drmsink_video_memory";
  alloc->mem_map = (GstMemoryMapFunction) gst_drmsink_video_memory_map;
  alloc->mem_unmap = (GstMemoryUnmapFunction) gst_drmsink_video_memory_unmap;
}

static GstAllocator *
gst_drmsink_video_memory_allocator_new (GstFramebufferSink *framebuffersink,
GstVideoInfo *info, gboolean pannable, gboolean is_overlay)
{
  GstDrmsink *drmsink = GST_DRMSINK (framebuffersink);
  GstDrmSinkVideoMemoryAllocator *drmsink_video_memory_allocator =
      g_object_new (gst_drmsink_video_memory_allocator_get_type (), NULL);
  gchar s[128];
  gchar *str;
  drmsink_video_memory_allocator->drmsink = drmsink;
  drmsink_video_memory_allocator->w = GST_VIDEO_INFO_WIDTH (info);
  drmsink_video_memory_allocator->h = GST_VIDEO_INFO_HEIGHT (info);
  drmsink_video_memory_allocator->format_info = *(GstVideoFormatInfo *)info->finfo;
  drmsink_video_memory_allocator->total_allocated = 0;
  g_sprintf (s, "drmsink_video_memory_%p", drmsink_video_memory_allocator);
  gst_allocator_register (s, gst_object_ref (drmsink_video_memory_allocator) );
  str = g_strdup_printf ("Created video memory allocator %s, %dx%d, format %s",
      s, drmsink_video_memory_allocator->w, drmsink_video_memory_allocator->h,
      gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (info)));
  GST_DRMSINK_INFO_OBJECT (drmsink, str);
  g_free (str);
  return GST_ALLOCATOR_CAST (drmsink_video_memory_allocator);
}

static void
gst_drmsink_pan_display (GstFramebufferSink *framebuffersink,
    GstMemory *memory)
{
  GstDrmsink *drmsink = GST_DRMSINK (framebuffersink);
  GstDrmSinkVideoMemory *vmem = (GstDrmSinkVideoMemory *)memory;
  fd_set fds;
  struct timeval tv;
  uint32_t connectors[1];
  gchar *s;

#if 0
  s = g_strdup_printf ("pan_display called, mem = %p, map_address = %p",
      vmem, vmem->map_address);
  GST_DRMSINK_INFO_OBJECT (drmsink, s);
  g_free (s);
#endif

  if (!drmsink->crtc_mode_initialized) {
    connectors[0] = drmsink->connector_id;
    if (drmModeSetCrtc (drmsink->fd, drmsink->crtc_id, vmem->fb,
        0, 0, connectors, 1, &drmsink->mode)) {
      GST_DRMSINK_INFO_OBJECT (drmsink, "drmModeSetCrtc failed");
      return;
    }
    drmsink->crtc_mode_initialized = TRUE;
  }

  drmsink->page_flip_occurred = FALSE;
  if (drmModePageFlip (drmsink->fd, drmsink->crtc_id, vmem->fb, DRM_MODE_PAGE_FLIP_EVENT,
      drmsink)) {
    GST_DRMSINK_INFO_OBJECT (drmsink, "drmModePageFlip failed");
    return;
  }
  memset (&tv, 0, sizeof (tv));
  FD_ZERO (&fds);
  while (TRUE) {
    FD_SET (drmsink->fd, &fds);
    tv.tv_sec = 5;
    select (drmsink->fd + 1, &fds, NULL, NULL, &tv);
    if (FD_ISSET (drmsink->fd, &fds)) {
      drmHandleEvent(drmsink->fd, drmsink->event_context);
      if (drmsink->page_flip_occurred)
          break;
    }
  }
}

static void
gst_drmsink_wait_for_vsync (GstFramebufferSink *framebuffersink)
{
  GstDrmsink *drmsink = GST_DRMSINK (framebuffersink);
  drmVBlank vbl;
  fd_set fds;
  struct timeval tv;

  GST_DRMSINK_INFO_OBJECT (drmsink, "wait_for_vsync called");

  drmsink->vblank_occurred = FALSE;
  vbl.request.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT;
  vbl.request.sequence = 1;
  drmWaitVBlank(drmsink->fd, &vbl);
#if 0
  memset (&tv, 0, sizeof (tv));
  FD_ZERO (&fds);
  while (TRUE) {
    FD_SET (drmsink->fd, &fds);
    tv.tv_sec = 5;
    select (drmsink->fd + 1, &fds, NULL, NULL, &tv);
    if (FD_ISSET (drmsink->fd, &fds)) {
      drmHandleEvent(drmsink->fd, drmsink->event_context);
      if (drmsink->vblank_occurred)
          break;
    }
  }
#endif
}
