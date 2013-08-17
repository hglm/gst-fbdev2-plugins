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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-sunxifbsink
 *
 * The sunxifbsink element intends implements a hardware accelerated
 * video sink for the console framebuffer of Allwinner A1x/A20-based
 * devices. The basis of the implementation is the optimized fbdev
 * sink as implemented in the GstFbdevFramebufferSink class.
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
 * gst-launch -v videotestsrc ! sunxifbsink full-screen=true
 * ]|
 * Run videotstsrc at full-screen resolution
 * |[
 * gst-launch -v videotestsrc horizontal_speed=10 ! sunxifbsink \
 * full-screen=true buffer-pool=true graphics-mode=true
 * ]|
 * This command illustrates some of the plugin's optimization features
 * by rendering to video memory with vsync and page flipping in
 * console graphics mode. There should be no tearing with page flipping/
 * vsync enabled. You might have to use the fps property to reduce the frame
 * rate on slower systems.
 * |[
 * gst-launch playbin uri=[uri] video-sink="sunxifbsink full-screen=true"
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

static inline void GST_SUNXIFBSINK_MESSAGE_OBJECT (GstSunxifbsink * sunxifbsink,
const gchar *message) {
  if (!sunxifbsink->fbdevframebuffersink.framebuffersink.silent)
    g_print ("%s.\n", message);
  else
    GST_INFO_OBJECT (sunxifbsink, message);
}

#define ALIGNMENT_GET_ALIGN_BYTES(offset, align) \
    (((align) + 1 - ((offset) & (align))) & (align))
#define ALIGNMENT_GET_ALIGNED(offset, align) \
    ((offset) + ALIGNMENT_GET_ALIGN_BYTES(offset, align))
#define ALIGNMENT_APPLY(offset, align) \
    offset = ALIGNMENT_GET_ALIGNED(offset, align);

/* Class function prototypes. */
static gboolean gst_sunxifbsink_open_hardware (GstFramebufferSink *framebuffersink,
    GstVideoInfo *info, gsize *video_memory_size, gsize *pannable_video_memory_size);
static void gst_sunxifbsink_close_hardware (GstFramebufferSink *framebuffersink);
static GstVideoFormat *gst_sunxifbsink_get_supported_overlay_formats (GstFramebufferSink *framebuffersink);
static gboolean gst_sunxifbsink_get_overlay_video_alignment (GstFramebufferSink *framebuffersink,
    GstVideoInfo *video_info, GstFramebufferSinkOverlayVideoAlignment *video_alignment, gint *overlay_align,
    gboolean *video_alignment_matches);
static gboolean gst_sunxifbsink_prepare_overlay (GstFramebufferSink *framebuffersink, GstVideoFormat format);
static GstFlowReturn gst_sunxifbsink_show_overlay (GstFramebufferSink *framebuffersink,
    GstMemory *memory);

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
        "; " GST_VIDEO_CAPS_MAKE ("YUY2") \
        "; " GST_VIDEO_CAPS_MAKE ("UYVY") \
        "; " GST_VIDEO_CAPS_MAKE ("Y444") \
        "; " GST_VIDEO_CAPS_MAKE ("AYUV") \
        "; " GST_VIDEO_CAPS_MAKE ("I420") \
        "; " GST_VIDEO_CAPS_MAKE ("YV12") \
        "; " GST_VIDEO_CAPS_MAKE ("NV12") \
        "; " GST_VIDEO_CAPS_MAKE ("NV21") ", " \
        "framerate = (fraction) [ 0, MAX ], " \
        "width = (int) [ 1, MAX ], " "height = (int) [ 1, MAX ]"

static GstStaticPadTemplate gst_sunxifbsink_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_SUNXIFBSINK_TEMPLATE_CAPS)
    );

static GstVideoFormat sunxifbsink_supported_overlay_formats_table[] = {
  /* List the formats that support odds widths first. */
  GST_VIDEO_FORMAT_YUY2,
  GST_VIDEO_FORMAT_UYVY,
  GST_VIDEO_FORMAT_Y444,
  GST_VIDEO_FORMAT_AYUV,
  GST_VIDEO_FORMAT_BGRx,
  /* These formats do not properly support odd widths. */
  GST_VIDEO_FORMAT_I420,
  GST_VIDEO_FORMAT_YV12,
  GST_VIDEO_FORMAT_NV12,
  GST_VIDEO_FORMAT_NV21,
  GST_VIDEO_FORMAT_UNKNOWN
};

/* Class initialization. */

#define gst_sunxifbsink_parent_class fbdevframebuffersink_parent_class
G_DEFINE_TYPE_WITH_CODE (GstSunxifbsink, gst_sunxifbsink, GST_TYPE_FBDEVFRAMEBUFFERSINK,
  GST_DEBUG_CATEGORY_INIT (gst_sunxifbsink_debug_category, "sunxifbsink", 0,
  "debug category for sunxifbsink element"));

static void
gst_sunxifbsink_class_init (GstSunxifbsinkClass* klass)
{
//  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
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
  framebuffer_sink_class->get_supported_overlay_formats = GST_DEBUG_FUNCPTR (gst_sunxifbsink_get_supported_overlay_formats);
  framebuffer_sink_class->get_overlay_video_alignment = GST_DEBUG_FUNCPTR (
      gst_sunxifbsink_get_overlay_video_alignment);
  framebuffer_sink_class->prepare_overlay = GST_DEBUG_FUNCPTR (gst_sunxifbsink_prepare_overlay);
  framebuffer_sink_class->show_overlay = GST_DEBUG_FUNCPTR (gst_sunxifbsink_show_overlay);
}

/* Class member functions. */

static void
gst_sunxifbsink_init (GstSunxifbsink *sunxifbsink) {
}

static gboolean
gst_sunxifbsink_open_hardware (GstFramebufferSink *framebuffersink, GstVideoInfo *info,
gsize *video_memory_size, gsize *pannable_video_memory_size) {
  GstSunxifbsink *sunxifbsink = GST_SUNXIFBSINK (framebuffersink);
  GstFbdevFramebufferSink *fbdevframebuffersink = GST_FBDEVFRAMEBUFFERSINK (framebuffersink);
  int version;
  uint32_t tmp;

  if (!gst_fbdevframebuffersink_open_hardware(framebuffersink, info, video_memory_size,
      pannable_video_memory_size))
    return FALSE;

  sunxifbsink->fd_disp = open ("/dev/disp", O_RDWR);

  sunxifbsink->hardware_overlay_available = FALSE;

  if (framebuffersink->use_hardware_overlay == FALSE)
    return TRUE;

  if (sunxifbsink->fd_disp < 0)
    return TRUE;

  tmp = SUNXI_DISP_VERSION;
  version = ioctl (sunxifbsink->fd_disp, DISP_CMD_VERSION, &tmp);
  if (version < 0) {
    close(sunxifbsink->fd_disp);
    GST_SUNXIFBSINK_MESSAGE_OBJECT (sunxifbsink,
        "Could not open sunxi disp controller");
    return TRUE;
  }

  /* Get the ID of the screen layer. */
  if (ioctl (fbdevframebuffersink->fd, sunxifbsink->framebuffer_id == 0 ?
  FBIOGET_LAYER_HDL_0 : FBIOGET_LAYER_HDL_1, &sunxifbsink->gfx_layer_id)) {
    close(sunxifbsink->fd_disp);
    return TRUE;
  }

  if (!gst_sunxifbsink_reserve_layer(sunxifbsink)) {
    close(sunxifbsink->fd_disp);
    return TRUE;
  }

  sunxifbsink->layer_is_visible = FALSE;

  sunxifbsink->hardware_overlay_available = TRUE;
  GST_SUNXIFBSINK_MESSAGE_OBJECT (sunxifbsink, "Hardware overlay available");

  return TRUE;
}

static void
gst_sunxifbsink_close_hardware (GstFramebufferSink *framebuffersink) {
  GstSunxifbsink *sunxifbsink = GST_SUNXIFBSINK (framebuffersink);

  if (sunxifbsink->hardware_overlay_available) {
    gst_sunxifbsink_hide_layer(sunxifbsink);

    gst_sunxifbsink_release_layer(sunxifbsink);

    close(sunxifbsink->fd_disp);
  }

  gst_fbdevframebuffersink_close_hardware (framebuffersink);
}

static GstVideoFormat *
gst_sunxifbsink_get_supported_overlay_formats (GstFramebufferSink *framebuffersink)
{
  return sunxifbsink_supported_overlay_formats_table;
}

/* Return the video alignment (top/bottom/left/right padding and stride alignment for each plane) that
   is required to display the overlay described by video_info. Also returns the alignment requirement
   of the start address of the overlay in video memory. video_alignment_matches is set to TRUE if
   the alignment defined by video_info did not have to be adjusted, FALSE otherwise. The function
   returns TRUE if hardware overlay with given video info is supported, FALSE otherwise. */

gboolean
gst_sunxifbsink_get_overlay_video_alignment(GstFramebufferSink *framebuffersink, GstVideoInfo *video_info,
    GstFramebufferSinkOverlayVideoAlignment *video_alignment, gint *overlay_align,
    gboolean *video_alignment_matches)
{
  GstVideoFormat format;
  format = GST_VIDEO_INFO_FORMAT (video_info);
  if (format == GST_VIDEO_FORMAT_I420 ||
      format == GST_VIDEO_FORMAT_YV12 ||
      format == GST_VIDEO_FORMAT_NV12 ||
      format == GST_VIDEO_FORMAT_NV21) {
    if (GST_VIDEO_INFO_WIDTH (video_info) & 1)
      /* Hardware overlay not supported for odd widths for all planar formats except Y444.
         Although it almost works for odd widths, there is an artifact line at the right of the scaled
         area, related to the alignment requirements of the width. */
      return FALSE;
  }
  /* When uses other formats, some artifacts have been observed when the width is odd, but for now
     leave support for odd widths enabled. */
  *overlay_align = 15;
  /* For the Allwinner hardware overlay, scanlines need to be aligned to pixel boundaries with a minimum
     alignment of word-aligned. This is a good match for the buffer format generally provided by
     upstream, so direct video memory buffer pool streaming is almost always possible. */
  gst_framebuffersink_set_overlay_video_alignment_from_scanline_alignment (framebuffersink, video_info,
      3, TRUE, video_alignment, video_alignment_matches);
  return TRUE;
}

/*
 * For the prepare overlay and show overlay functions, the parameters are
 * stored in the following fields:
 * framebuffersink->overlay_plane_offset[i] is the offset in bytes of each plane. Any
 *   top or left padding returned by get_overlay_video_alignment() will come first.
 * framebuffersink->overlay_scanline_offset[i] is the offset in bytes of the first pixel of each
 *   scanline for each plane (corresponding with the left padding * bytes per pixel). Usually 0.
 * framebuffersink->overlay_scanline_stride[i] is the scanline stride in bytes of each plane.
 * framebuffersink->videosink.width is the source width.
 * framebuffersink->videosink.height is the source height.
 * framebuffersink->video_rectangle.x is the destination x coordinate.
 * framebuffersink->video_rectangle.y is the destination y coordinate.
 * framebuffersink->video_rectangle.w is the destination width.
 * framebuffersink->video_rectangle.h is the destination height.
 */

static gboolean
gst_sunxifbsink_prepare_overlay (GstFramebufferSink *framebuffersink, GstVideoFormat format)
{
  GstSunxifbsink *sunxifbsink = GST_SUNXIFBSINK (framebuffersink);

  if (sunxifbsink->layer_is_visible)
    gst_sunxifbsink_hide_layer(sunxifbsink);

  sunxifbsink->overlay_format = format;

  return TRUE;
}

static GstFlowReturn
gst_sunxifbsink_show_overlay_yuv_planar (GstFramebufferSink *framebuffersink, guintptr framebuffer_offset,
GstVideoFormat format)
{
  GstSunxifbsink *sunxifbsink = GST_SUNXIFBSINK (framebuffersink);
  GstFbdevFramebufferSink *fbdevframebuffersink = GST_FBDEVFRAMEBUFFERSINK (framebuffersink);
    __disp_fb_t fb;
    __disp_rect_t rect;
    __disp_rect_t output_rect;
    uint32_t tmp[4];

    memset(&fb, 0, sizeof (fb));

    if (format == GST_VIDEO_FORMAT_Y444) {
      fb.addr[0] = fbdevframebuffersink->fixinfo.smem_start + framebuffer_offset;
      fb.addr[1] = fbdevframebuffersink->fixinfo.smem_start + framebuffer_offset +
          framebuffersink->overlay_plane_offset[1];
      fb.addr[2] = fbdevframebuffersink->fixinfo.smem_start + framebuffer_offset +
          framebuffersink->overlay_plane_offset[2];
      fb.format = DISP_FORMAT_YUV444;
      fb.seq = DISP_SEQ_P3210;
      fb.mode = DISP_MOD_NON_MB_PLANAR;
    }
    else if (format == GST_VIDEO_FORMAT_NV12 || format == GST_VIDEO_FORMAT_NV21) {
      fb.addr[0] = fbdevframebuffersink->fixinfo.smem_start + framebuffer_offset;
      fb.addr[1] = fbdevframebuffersink->fixinfo.smem_start + framebuffer_offset +
          framebuffersink->overlay_plane_offset[1];
      fb.format = DISP_FORMAT_YUV420;
      if (format == GST_VIDEO_FORMAT_NV12)
        fb.seq = DISP_SEQ_UVUV;
      else
        fb.seq = DISP_SEQ_VUVU;
      fb.mode = DISP_MOD_NON_MB_UV_COMBINED;
    }
    else {
      fb.addr[0] = fbdevframebuffersink->fixinfo.smem_start + framebuffer_offset;
      if (format == GST_VIDEO_FORMAT_I420) {
        fb.addr[1] = fbdevframebuffersink->fixinfo.smem_start + framebuffer_offset +
            framebuffersink->overlay_plane_offset[1];
        fb.addr[2] = fbdevframebuffersink->fixinfo.smem_start + framebuffer_offset +
            framebuffersink->overlay_plane_offset[2];
      }
      else {
        /* GST_VIDEO_FORMAT_YV12 */
        fb.addr[1] = fbdevframebuffersink->fixinfo.smem_start + framebuffer_offset +
            framebuffersink->overlay_plane_offset[2];
        fb.addr[2] = fbdevframebuffersink->fixinfo.smem_start + framebuffer_offset +
            framebuffersink->overlay_plane_offset[1];
      }
      fb.format = DISP_FORMAT_YUV420;
      fb.seq = DISP_SEQ_P3210;
      fb.mode = DISP_MOD_NON_MB_PLANAR;
    }
    fb.size.width = framebuffersink->overlay_scanline_stride[0]
        / (GST_VIDEO_FORMAT_INFO_SCALE_WIDTH (framebuffersink->video_info.finfo, 0, 8)
        * GST_VIDEO_INFO_COMP_PSTRIDE (&framebuffersink->video_info, 0) / 8);
    fb.size.height = framebuffersink->videosink.height;

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

    output_rect.x = framebuffersink->video_rectangle.x;
    output_rect.y = framebuffersink->video_rectangle.y;
    output_rect.width = framebuffersink->video_rectangle.w;
    output_rect.height = framebuffersink->video_rectangle.h;
    tmp[0] = sunxifbsink->framebuffer_id;
    tmp[1] = sunxifbsink->layer_id;
    tmp[2] = (uintptr_t)&output_rect;
    if (ioctl(sunxifbsink->fd_disp, DISP_CMD_LAYER_SET_SCN_WINDOW, &tmp) < 0)
        return GST_FLOW_ERROR;

    gst_sunxifbsink_show_layer(sunxifbsink);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_sunxifbsink_show_overlay_yuv_packed (GstFramebufferSink *framebuffersink, guintptr framebuffer_offset,
GstVideoFormat format)
{
  GstSunxifbsink *sunxifbsink = GST_SUNXIFBSINK (framebuffersink);
  GstFbdevFramebufferSink *fbdevframebuffersink = GST_FBDEVFRAMEBUFFERSINK (framebuffersink);
    __disp_fb_t fb;
    __disp_rect_t rect;
    __disp_rect_t output_rect;
    uint32_t tmp[4];

    memset(&fb, 0, sizeof (fb));

    fb.addr[0] = fbdevframebuffersink->fixinfo.smem_start + framebuffer_offset;
    fb.size.height = framebuffersink->videosink.height;
    if (format == GST_VIDEO_FORMAT_AYUV) {
      fb.format = DISP_FORMAT_YUV444;
      fb.seq = DISP_SEQ_AYUV;
    }
    else {
      fb.format = DISP_FORMAT_YUV422;
      if (format == GST_VIDEO_FORMAT_YUY2)
        fb.seq = DISP_SEQ_YUYV;
      else
        fb.seq = DISP_SEQ_UYVY;
    }
    fb.size.width = framebuffersink->overlay_scanline_stride[0]
        / (GST_VIDEO_FORMAT_INFO_SCALE_WIDTH (framebuffersink->video_info.finfo, 0, 8)
        * GST_VIDEO_INFO_COMP_PSTRIDE (&framebuffersink->video_info, 0) / 8);
    fb.mode = DISP_MOD_INTERLEAVED;

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

    output_rect.x = framebuffersink->video_rectangle.x;
    output_rect.y = framebuffersink->video_rectangle.y;
    output_rect.width = framebuffersink->video_rectangle.w;
    output_rect.height = framebuffersink->video_rectangle.h;
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
  GstFbdevFramebufferSink *fbdevframebuffersink = GST_FBDEVFRAMEBUFFERSINK (framebuffersink);
    __disp_fb_t fb;
    __disp_rect_t rect;
    __disp_rect_t output_rect;
    uint32_t tmp[4];

    memset(&fb, 0, sizeof(fb));

    /* BGRX layer. */
    fb.addr[0] = fbdevframebuffersink->fixinfo.smem_start + framebuffer_offset;
    fb.size.width = framebuffersink->overlay_scanline_stride[0] >> 2;
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

    output_rect.x = framebuffersink->video_rectangle.x;
    output_rect.y = framebuffersink->video_rectangle.y;
    output_rect.width = framebuffersink->video_rectangle.w;
    output_rect.height = framebuffersink->video_rectangle.h;
    tmp[0] = sunxifbsink->framebuffer_id;
    tmp[1] = sunxifbsink->layer_id;
    tmp[2] = (uintptr_t)&output_rect;
    if (ioctl (sunxifbsink->fd_disp, DISP_CMD_LAYER_SET_SCN_WINDOW, tmp) < 0)
        return GST_FLOW_ERROR;

    gst_sunxifbsink_show_layer(sunxifbsink);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_sunxifbsink_show_overlay (GstFramebufferSink *framebuffersink, GstMemory *memory)
{
  GstSunxifbsink *sunxifbsink = GST_SUNXIFBSINK (framebuffersink);
  GstFbdevFramebufferSink *fbdevframebuffersink = GST_FBDEVFRAMEBUFFERSINK (framebuffersink);
  GstMapInfo mapinfo;
  guintptr framebuffer_offset;
  GstFlowReturn res;

  gst_memory_map(memory, &mapinfo, 0);
  framebuffer_offset = mapinfo.data - fbdevframebuffersink->framebuffer;

  GST_LOG_OBJECT (sunxifbsink, "Show overlay called (offset = 0x%08X)", framebuffer_offset);

  res = GST_FLOW_ERROR;
  if (sunxifbsink->overlay_format == GST_VIDEO_FORMAT_I420 ||
      sunxifbsink->overlay_format == GST_VIDEO_FORMAT_YV12 ||
      sunxifbsink->overlay_format == GST_VIDEO_FORMAT_Y444 ||
      sunxifbsink->overlay_format == GST_VIDEO_FORMAT_NV12 ||
      sunxifbsink->overlay_format == GST_VIDEO_FORMAT_NV21)
    res =  gst_sunxifbsink_show_overlay_yuv_planar (framebuffersink, framebuffer_offset,
        sunxifbsink->overlay_format);
  else if (sunxifbsink->overlay_format == GST_VIDEO_FORMAT_YUY2 ||
      sunxifbsink->overlay_format == GST_VIDEO_FORMAT_UYVY ||
      sunxifbsink->overlay_format == GST_VIDEO_FORMAT_AYUV)
    res =  gst_sunxifbsink_show_overlay_yuv_packed (framebuffersink, framebuffer_offset,
        sunxifbsink->overlay_format);
  else if (sunxifbsink->overlay_format == GST_VIDEO_FORMAT_BGRx)
    res = gst_sunxifbsink_show_overlay_bgrx32 (framebuffersink, framebuffer_offset);

  gst_memory_unmap(memory, &mapinfo);
  return res;
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
    layer_info.fb.addr[0] = sunxifbsink->fbdevframebuffersink.fixinfo.smem_start;
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

