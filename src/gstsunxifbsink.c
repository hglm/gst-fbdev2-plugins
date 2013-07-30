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

/*
 * The hardware addressing portion of the plugin was adapted from
 * xf86-video-sunxifb, which has the following copyright message;
 *
 * Copyright © 2013 Siarhei Siamashka <siarhei.siamashka@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
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

static gboolean gst_sunxifbsink_open_hardware (GstFramebufferSink *framebuffersink);
static void gst_sunxifbsink_close_hardware (GstFramebufferSink *framebuffersink);
static void gst_sunxifbsink_get_supported_overlay_types (GstFramebufferSink *framebuffersink, uint8_t *types);
static gboolean gst_sunxifbsink_prepare_overlay (GstFramebufferSink *framebuffersink, GstFramebufferSinkOverlayType t);
static GstFlowReturn gst_sunxifbsink_show_overlay (GstFramebufferSink *framebuffersink, guintptr framebuffer_offset);

static gboolean gst_sunxifbsink_reserve_layer(GstSunxifbsink *sunxifbsink);
static void gst_sunxifbsink_release_layer(GstSunxifbsink *sunxifbsink);
static gboolean gst_sunxifbsink_show_layer(GstSunxifbsink *sunxifbsink);
static void gst_sunxifbsink_hide_layer(GstSunxifbsink *sunxifbsink);

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
        "; " GST_VIDEO_CAPS_MAKE ("xBGR") \
        "; " GST_VIDEO_CAPS_MAKE ("I420") ", " \
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
  GstFramebufferSinkClass *framebuffer_sink_class = GST_FRAMEBUFFERSINK_CLASS (klass);

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

  framebuffer_sink_class->open_hardware = GST_DEBUG_FUNCPTR (gst_sunxifbsink_open_hardware);
  framebuffer_sink_class->close_hardware = GST_DEBUG_FUNCPTR (gst_sunxifbsink_close_hardware);
  framebuffer_sink_class->get_supported_overlay_types = GST_DEBUG_FUNCPTR (gst_sunxifbsink_get_supported_overlay_types);
  framebuffer_sink_class->prepare_overlay = GST_DEBUG_FUNCPTR (gst_sunxifbsink_prepare_overlay);
  framebuffer_sink_class->show_overlay = GST_DEBUG_FUNCPTR (gst_sunxifbsink_show_overlay);
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
gst_sunxifbsink_open_hardware (GstFramebufferSink *framebuffersink) {
  GstSunxifbsink *sunxifbsink = GST_SUNXIFBSINK (framebuffersink);
  int version;
  uint32_t tmp;

  sunxifbsink->fd_disp = open ("/dev/disp", O_RDWR);

  if (sunxifbsink->fd_disp < 0)
    return FALSE;

  tmp = SUNXI_DISP_VERSION;
  version = ioctl (sunxifbsink->fd_disp, DISP_CMD_VERSION, &tmp);
  if (version < 0) {
    close(sunxifbsink->fd_disp);
    GST_SUNXIFBSINK_INFO_OBJECT (sunxifbsink,
        "Could not open sunxi disp controller");
    return FALSE;
  }

  /* Get the ID of the screen layer. */
  if (ioctl (framebuffersink->fd, sunxifbsink->framebuffer_id == 0 ?
  FBIOGET_LAYER_HDL_0 : FBIOGET_LAYER_HDL_1, &sunxifbsink->gfx_layer_id)) {
    close(sunxifbsink->fd_disp);
    return FALSE;
  }

  if (!gst_sunxifbsink_reserve_layer(sunxifbsink)) {
    close(sunxifbsink->fd_disp);
    return FALSE;
  }

  sunxifbsink->layer_is_visible = FALSE;

  GST_SUNXIFBSINK_INFO_OBJECT (sunxifbsink, "Hardware overlay available");

  return TRUE;
}

static void
gst_sunxifbsink_close_hardware (GstFramebufferSink *framebuffersink) {
  GstSunxifbsink *sunxifbsink = GST_SUNXIFBSINK (framebuffersink);

  gst_sunxifbsink_hide_layer(sunxifbsink);

  gst_sunxifbsink_release_layer(sunxifbsink);

  close(sunxifbsink->fd_disp);

}

static void
gst_sunxifbsink_get_supported_overlay_types (GstFramebufferSink *framebuffersink, uint8_t *types)
{
  types[GST_FRAMEBUFFERSINK_OVERLAY_TYPE_I420] = 0;
  types[GST_FRAMEBUFFERSINK_OVERLAY_TYPE_BGRX32] = 1;
}

/* For the prepare overlay and show overlay functions, the output parameters are */
/* stored in the following fields: */
/* framebuffersink->videosink.width is the source width. */
/* framebuffersink->videosink.height is the source height. */
/* framebuffersink->cx is the destination x coordinate. */
/* framebuffersink->cy is the destination y coordinate. */
/* framebuffersink->scaled_width is the destination width. */
/* framebuffersink->scaled_height is the destination height. */

static gboolean
gst_sunxifbsink_prepare_overlay (GstFramebufferSink *framebuffersink, GstFramebufferSinkOverlayType t)
{
  GstSunxifbsink *sunxifbsink = GST_SUNXIFBSINK (framebuffersink);

  if (sunxifbsink->layer_is_visible)
    gst_sunxifbsink_hide_layer(sunxifbsink);

  return TRUE;
}

static GstFlowReturn
gst_sunxifbsink_show_overlay_I420 (GstFramebufferSink *framebuffersink, guintptr framebuffer_offset)
{
  GstSunxifbsink *sunxifbsink = GST_SUNXIFBSINK (framebuffersink);
    __disp_fb_t fb;
    __disp_rect_t rect;
    __disp_rect_t output_rect;
    uint32_t tmp[4];

    int stride = 1; /* DEBUG */

    memset(&fb, 0, sizeof (fb));

    fb.addr[0] = framebuffersink->fixinfo.smem_start + sunxifbsink->y_offset_in_framebuffer;
    fb.addr[1] = framebuffersink->fixinfo.smem_start + sunxifbsink->u_offset_in_framebuffer;
    fb.addr[2] = framebuffersink->fixinfo.smem_start + sunxifbsink->v_offset_in_framebuffer;
    fb.size.width = stride;
    fb.size.height = framebuffersink->videosink.height;
    fb.format = DISP_FORMAT_YUV420;
    fb.seq = DISP_SEQ_P3210;
    fb.mode = DISP_MOD_NON_MB_PLANAR;

    tmp[0] = sunxifbsink->framebuffer_id;
    tmp[1] = sunxifbsink->layer_id;
    tmp[2] = (uintptr_t)&fb;
    if (ioctl(sunxifbsink->fd_disp, DISP_CMD_LAYER_SET_FB, tmp) < 0)
        return GST_FLOW_ERROR;

    rect.x = 0;
    rect.y = 0;
    rect.width = framebuffersink->videosink.width;
    rect.height = framebuffersink->videosink.height;

    tmp[0] = sunxifbsink->framebuffer_id;
    tmp[1] = sunxifbsink->layer_id;
    tmp[2] = (uintptr_t)&rect;
    if (ioctl (sunxifbsink->fd_disp, DISP_CMD_LAYER_SET_SRC_WINDOW, tmp) < 0)
        return GST_FLOW_ERROR;

    output_rect.x = framebuffersink->cx;
    output_rect.y = framebuffersink->cy;
    output_rect.width = framebuffersink->scaled_width;
    output_rect.height = framebuffersink->scaled_height;
    tmp[0] = sunxifbsink->framebuffer_id;
    tmp[1] = sunxifbsink->layer_id;
    tmp[2] = (uintptr_t)&output_rect;
    if (ioctl(sunxifbsink->fd_disp, DISP_CMD_LAYER_SET_SCN_WINDOW, &tmp) < 0)
        return GST_FLOW_ERROR;

    gst_sunxifbsink_show_layer(sunxifbsink);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_sunxifbsink_show_overlay_bgrx32 (GstFramebufferSink *framebuffersink, guintptr framebuffer_offset)
{
  GstSunxifbsink *sunxifbsink = GST_SUNXIFBSINK (framebuffersink);
    __disp_fb_t fb;
    __disp_rect_t rect;
    __disp_rect_t output_rect;
    uint32_t tmp[4];

    memset(&fb, 0, sizeof(fb));

    /* BGRX layer. */
    fb.addr[0] = framebuffersink->fixinfo.smem_start + framebuffer_offset;
    fb.size.width = framebuffersink->videosink.width;
    fb.size.height = framebuffersink->videosink.height;
    fb.format = DISP_FORMAT_ARGB8888;
    fb.seq = DISP_SEQ_ARGB;
    fb.mode = DISP_MOD_INTERLEAVED;

    tmp[0] = sunxifbsink->framebuffer_id;
    tmp[1] = sunxifbsink->layer_id;
    tmp[2] = (uintptr_t)&fb;
    if (ioctl (sunxifbsink->fd_disp, DISP_CMD_LAYER_SET_FB, tmp) < 0)
        return GST_FLOW_ERROR;

    rect.x = 0;
    rect.y = 0;
    rect.width = framebuffersink->videosink.width;
    rect.height = framebuffersink->videosink.height;

    tmp[0] = sunxifbsink->framebuffer_id;
    tmp[1] = sunxifbsink->layer_id;
    tmp[2] = (uintptr_t)&rect;
    if (ioctl (sunxifbsink->fd_disp, DISP_CMD_LAYER_SET_SRC_WINDOW, tmp) < 0)
        return GST_FLOW_ERROR;

    output_rect.x = framebuffersink->cx;
    output_rect.y = framebuffersink->cy;
    output_rect.width = framebuffersink->scaled_width;
    output_rect.height = framebuffersink->scaled_height;
    tmp[0] = sunxifbsink->framebuffer_id;
    tmp[1] = sunxifbsink->layer_id;
    tmp[2] = (uintptr_t)&output_rect;
    if (ioctl (sunxifbsink->fd_disp, DISP_CMD_LAYER_SET_SCN_WINDOW, tmp) < 0)
        return GST_FLOW_ERROR;

    gst_sunxifbsink_show_layer(sunxifbsink);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_sunxifbsink_show_overlay (GstFramebufferSink *framebuffersink, guintptr framebuffer_offset)
{
  GstSunxifbsink *sunxifbsink = GST_SUNXIFBSINK (framebuffersink);

#if 0
  char s[80];
  sprintf(s, "Show overlay called (offset = 0x%08X)", framebuffer_offset);
  GST_SUNXIFBSINK_INFO_OBJECT(sunxifbsink, s);
#endif

  if (GST_VIDEO_INFO_FORMAT (&framebuffersink->info) == GST_VIDEO_FORMAT_I420)
    return gst_sunxifbsink_show_overlay_I420 (framebuffersink, framebuffer_offset);
  if (GST_VIDEO_INFO_FORMAT (&framebuffersink->info) == GST_VIDEO_FORMAT_BGRx)
    return gst_sunxifbsink_show_overlay_bgrx32 (framebuffersink, framebuffer_offset);
  return GST_FLOW_ERROR;
}

static gboolean
gst_sunxifbsink_reserve_layer(GstSunxifbsink *sunxifbsink) {
    __disp_layer_info_t layer_info;
    uint32_t tmp[4];

    /* try to allocate a layer */

    tmp[0] = sunxifbsink->framebuffer_id;
    tmp[1] = DISP_LAYER_WORK_MODE_NORMAL;
    sunxifbsink->layer_id = ioctl (sunxifbsink->fd_disp, DISP_CMD_LAYER_REQUEST, &tmp);
    if (sunxifbsink->layer_id < 0)
        return FALSE;

    /* also try to enable scaler for this layer */

    tmp[0] = sunxifbsink->framebuffer_id;
    tmp[1] = sunxifbsink->layer_id;
    tmp[2] = (uintptr_t)&layer_info;
    if (ioctl (sunxifbsink->fd_disp, DISP_CMD_LAYER_GET_PARA, tmp) < 0)
        return FALSE;

    layer_info.mode      = DISP_LAYER_WORK_MODE_SCALER;
    /* the screen and overlay layers need to be in different pipes */
    layer_info.pipe      = 1;
    layer_info.alpha_en  = 1;
    layer_info.alpha_val = 255;

    /* Initially set "layer_info.fb" to something reasonable in order to avoid
     * "[DISP] not supported scaler input pixel format:0 in Scaler_sw_para_to_reg1"
     * warning in dmesg log */
    layer_info.fb.addr[0] = sunxifbsink->framebuffersink.fixinfo.smem_start;
    layer_info.fb.size.width = 1;
    layer_info.fb.size.height = 1;
    layer_info.fb.format = DISP_FORMAT_ARGB8888;
    layer_info.fb.seq = DISP_SEQ_ARGB;
    layer_info.fb.mode = DISP_MOD_INTERLEAVED;

    tmp[0] = sunxifbsink->framebuffer_id;
    tmp[1] = sunxifbsink->layer_id;
    tmp[2] = (uintptr_t)&layer_info;

    if (ioctl (sunxifbsink->fd_disp, DISP_CMD_LAYER_SET_PARA, tmp) >= 0)
        sunxifbsink->layer_has_scaler = TRUE;

  return TRUE;
}

static void
gst_sunxifbsink_release_layer(GstSunxifbsink *sunxifbsink) {
    uint32_t tmp[4];

    tmp[0] = sunxifbsink->framebuffer_id;
    tmp[1] = sunxifbsink->layer_id;
    ioctl (sunxifbsink->fd_disp, DISP_CMD_LAYER_RELEASE, tmp);

    sunxifbsink->layer_id = -1;
    sunxifbsink->layer_has_scaler = 0;
}

static gboolean gst_sunxifbsink_show_layer(GstSunxifbsink *sunxifbsink) {
  uint32_t tmp[4];

  if (sunxifbsink->layer_is_visible)
    return TRUE;

  if (sunxifbsink->layer_id < 0)
    return FALSE;

  tmp[0] = sunxifbsink->framebuffer_id;
  tmp[1] = sunxifbsink->layer_id;
  if (ioctl(sunxifbsink->fd_disp, DISP_CMD_LAYER_OPEN, &tmp) < 0)
    return FALSE;

  sunxifbsink->layer_is_visible = TRUE;
  return TRUE;
}

static void gst_sunxifbsink_hide_layer(GstSunxifbsink *sunxifbsink) {
  uint32_t tmp[4];

  if (!sunxifbsink->layer_is_visible)
    return;

  tmp[0] = sunxifbsink->framebuffer_id;
  tmp[1] = sunxifbsink->layer_id;
  ioctl(sunxifbsink->fd_disp, DISP_CMD_LAYER_CLOSE, &tmp);

  sunxifbsink->layer_is_visible = FALSE;
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

