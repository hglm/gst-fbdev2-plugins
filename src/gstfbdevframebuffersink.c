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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:class-GstFbdevFramebufferSink
 *
 * The GstFbdevFramebufferSink class implements an optimized video sink
 * for the Linux console framebuffer, derived from GstFramebufferSink.
 * It is used as the basis for the fbdev2sink plugin. It can write directly
 * into video memory with page flipping support, and should be usable by
 * a wide variety of devices. The class can be derived for device-specific
 * implementations with hardware acceleration.
 *
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
#include <inttypes.h>
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
#include "gstfbdevframebuffersink.h"

GST_DEBUG_CATEGORY_STATIC (gst_fbdevframebuffersink_debug_category);
#define GST_CAT_DEFAULT gst_fbdevframebuffersink_debug_category

/* Function to produce informational message if silent property is not set; */
/* if the silent property is enabled only debugging info is produced. */
static void GST_FBDEVFRAMEBUFFERSINK_MESSAGE_OBJECT (GstFbdevFramebufferSink *fbdevframebuffersink,
const gchar *message) {
  if (!fbdevframebuffersink->framebuffersink.silent)
    g_print ("%s.\n", message);
  else
    GST_INFO_OBJECT (fbdevframebuffersink, message);
}

#define ALIGNMENT_GET_ALIGN_BYTES(offset, align) \
    (((align) + 1 - ((offset) & (align))) & (align))
#define ALIGNMENT_GET_ALIGNED(offset, align) \
    ((offset) + ALIGNMENT_GET_ALIGN_BYTES(offset, align))
#define ALIGNMENT_APPLY(offset, align) \
    offset = ALIGNMENT_GET_ALIGNED(offset, align);

/* Class function prototypes. */
static GstAllocator *gst_fbdevframebuffersink_video_memory_allocator_new (
    GstFramebufferSink *framebuffersink, GstVideoInfo *info, gboolean pannable,
    gboolean is_overlay);
static void gst_fbdevframebuffersink_pan_display (GstFramebufferSink *framebuffersink,
    GstMemory *memory);
static void gst_fbdevframebuffersink_wait_for_vsync (GstFramebufferSink *framebuffersink);

/* Local functions. */
static void gst_fbdevframebuffersink_pan_display_fbdev (GstFbdevFramebufferSink *fbdevframebuffersink,
    int x, int y);

/* Standard video memory implementation. */
static void gst_fbdevframebuffersink_video_memory_init (gpointer framebuffer, gsize map_size);

enum
{
  PROP_0,
};

/* Class initialization. */

static void
gst_fbdevframebuffersink_class_init (GstFbdevFramebufferSinkClass* klass)
{
  GstFramebufferSinkClass *framebuffer_sink_class = GST_FRAMEBUFFERSINK_CLASS (klass);

  framebuffer_sink_class->open_hardware = GST_DEBUG_FUNCPTR (gst_fbdevframebuffersink_open_hardware);
  framebuffer_sink_class->close_hardware = GST_DEBUG_FUNCPTR (gst_fbdevframebuffersink_close_hardware);
  framebuffer_sink_class->video_memory_allocator_new = GST_DEBUG_FUNCPTR (gst_fbdevframebuffersink_video_memory_allocator_new);
  framebuffer_sink_class->pan_display = GST_DEBUG_FUNCPTR (gst_fbdevframebuffersink_pan_display);
  framebuffer_sink_class->wait_for_vsync = GST_DEBUG_FUNCPTR (gst_fbdevframebuffersink_wait_for_vsync);
}

static void
gst_fbdevframebuffersink_base_init (gpointer g_class)
{
  GST_DEBUG_CATEGORY_INIT (gst_fbdevframebuffersink_debug_category,
      "fbdevframebuffersink", 0, "GstFbdevFramebufferSink" );
}

/* Class member functions. */

static void
gst_fbdevframebuffersink_init (GstFbdevFramebufferSink *fbdevframebuffersink) {
  GstFramebufferSink *framebuffersink = GST_FRAMEBUFFERSINK (fbdevframebuffersink);

  fbdevframebuffersink->framebuffer = NULL;

  /* Override the default value of the device property from GstFramebufferSink. */
  framebuffersink->device = g_strdup ("/dev/fb0");
}

/* Helper function. */
static gboolean
gst_fbdevframebuffersink_set_device_virtual_size(GstFbdevFramebufferSink *fbdevframebuffersink,
    int xres, int yres)
{
  fbdevframebuffersink->varinfo.xres_virtual = xres;
  fbdevframebuffersink->varinfo.yres_virtual = yres;
  /* Set the variable screen info. */
  if (ioctl (fbdevframebuffersink->fd, FBIOPUT_VSCREENINFO, &fbdevframebuffersink->varinfo))
    return FALSE;
  /* Read back test. */
  ioctl (fbdevframebuffersink->fd, FBIOGET_VSCREENINFO, &fbdevframebuffersink->varinfo);
  if (fbdevframebuffersink->varinfo.yres_virtual != yres)
    return FALSE;
  return TRUE;
}

/* Helper function. */
static uint32_t
swapendian (uint32_t val)
{
  return (val & 0xff) << 24 | (val & 0xff00) << 8
      | (val & 0xff0000) >> 8 | (val & 0xff000000) >> 24;
}

/* The following member function is exported for use by derived subclasses. */
gboolean
gst_fbdevframebuffersink_open_hardware (GstFramebufferSink *framebuffersink,
    GstVideoInfo *info, gsize *video_memory_size, gsize *pannable_video_memory_size)
{
  GstFbdevFramebufferSink *fbdevframebuffersink = GST_FBDEVFRAMEBUFFERSINK (framebuffersink);
  struct fb_fix_screeninfo fixinfo;
  struct fb_var_screeninfo varinfo;
  uint32_t rmask;
  uint32_t gmask;
  uint32_t bmask;
  int endianness;
  int depth;
  GstVideoFormat framebuffer_format;
  GstVideoAlignment align;
  int max_framebuffers;

  fbdevframebuffersink->fd = open (framebuffersink->device, O_RDWR);

  if (fbdevframebuffersink->fd == -1)
    goto err;

  /* get the fixed screen info */
  if (ioctl (fbdevframebuffersink->fd, FBIOGET_FSCREENINFO, &fixinfo)) {
    close (fbdevframebuffersink->fd);
    goto err;
  }

  /* get the variable screen info */
  if (ioctl (fbdevframebuffersink->fd, FBIOGET_VSCREENINFO, &varinfo)) {
    close (fbdevframebuffersink->fd);
    goto err;
  }

  /* Map the framebuffer. */
  if (framebuffersink->max_video_memory_property == 0)
    /* Only allocate up to reported virtual size when the video-memory property is 0. */
    fbdevframebuffersink->framebuffer_map_size =
      fixinfo.line_length * varinfo.yres_virtual;
  else if (framebuffersink->max_video_memory_property == - 1) {
    /* Allocate up to 8 screens when the property is set to - 1. */
    fbdevframebuffersink->framebuffer_map_size =
        fixinfo.line_length * varinfo.yres * 8;
    if (fbdevframebuffersink->framebuffer_map_size > fixinfo.smem_len)
      fbdevframebuffersink->framebuffer_map_size = fixinfo.smem_len;
  }
  else if (framebuffersink->max_video_memory_property == - 2)
    /* Allocate all video memory when video-memory is set to - 2. */
    fbdevframebuffersink->framebuffer_map_size = fixinfo.smem_len;
  else {
     /* Use the setting from video-memory, but sanitize it. */
    fbdevframebuffersink->framebuffer_map_size =
        framebuffersink->max_video_memory_property * 1024 * 1024;
    if (fbdevframebuffersink->framebuffer_map_size > fixinfo.smem_len)
      fbdevframebuffersink->framebuffer_map_size = fixinfo.smem_len;
    if (fbdevframebuffersink->framebuffer_map_size < fixinfo.line_length
        * varinfo.yres)
      fbdevframebuffersink->framebuffer_map_size = fixinfo.line_length
          * varinfo.yres;
  }
  fbdevframebuffersink->framebuffer = mmap (0, fbdevframebuffersink->framebuffer_map_size,
      PROT_WRITE, MAP_SHARED, fbdevframebuffersink->fd, 0);
  if (fbdevframebuffersink->framebuffer == MAP_FAILED) {
    close (fbdevframebuffersink->fd);
    goto err;
  }

  *video_memory_size = fbdevframebuffersink->framebuffer_map_size;

  framebuffersink->nu_screens_used = 1;

  /* Check the pixel depth and determine the color masks. */
  rmask = ((1 << varinfo.red.length) - 1)
      << varinfo.red.offset;
  gmask = ((1 << varinfo.green.length) - 1)
      << varinfo.green.offset;
  bmask = ((1 << varinfo.blue.length) - 1)
      << varinfo.blue.offset;
  endianness = 0;

  switch (varinfo.bits_per_pixel) {
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
      varinfo.bits_per_pixel);
      goto err;
  }

  /* Set the framebuffer video format. */
  depth = varinfo.red.length + varinfo.green.length
      + varinfo.blue.length;

  framebuffer_format = gst_video_format_from_masks (depth, varinfo.bits_per_pixel,
      endianness, rmask, gmask, bmask, 0);

  gst_video_info_init (info);
  gst_video_info_set_format (info, framebuffer_format, varinfo.xres, varinfo.yres);
  gst_video_alignment_reset (&align);
  /* Set alignment to word boundaries. */
  align.stride_align[0] = 3;
  gst_video_info_align (info, &align);

  fbdevframebuffersink->fixinfo = fixinfo;
  fbdevframebuffersink->varinfo = varinfo;

  /* Make sure all framebuffers can be panned to. */
  max_framebuffers = fbdevframebuffersink->framebuffer_map_size / GST_VIDEO_INFO_SIZE (info);
  if (fbdevframebuffersink->varinfo.yres_virtual < max_framebuffers * GST_VIDEO_INFO_HEIGHT (info)
      && !gst_fbdevframebuffersink_set_device_virtual_size(fbdevframebuffersink,
      fbdevframebuffersink->varinfo.xres_virtual,
      framebuffersink->max_framebuffers * GST_VIDEO_INFO_HEIGHT (info))) {
    GST_FBDEVFRAMEBUFFERSINK_MESSAGE_OBJECT (fbdevframebuffersink,
        "Could not set the device virtual screen size large enough to support all buffers");
    *pannable_video_memory_size = fbdevframebuffersink->varinfo.yres_virtual *
        fbdevframebuffersink->fixinfo.line_length;
  }
  else
    *pannable_video_memory_size = max_framebuffers * GST_VIDEO_INFO_SIZE (info);

  /* Initialize video memory. */
  gst_fbdevframebuffersink_video_memory_init(fbdevframebuffersink->framebuffer,
      fbdevframebuffersink->framebuffer_map_size);

  {
    gchar *s = g_strdup_printf("Succesfully opened fbdev framebuffer device %s, "
        "mapped sized %td MB of which %" PRIu64 " MB (%d buffers) usable for page flipping",
        framebuffersink->device,
        fbdevframebuffersink->framebuffer_map_size / (1024 * 1024),
        (uint64_t)max_framebuffers * fixinfo.line_length *
        GST_VIDEO_INFO_HEIGHT (info) / (1024 * 1024), max_framebuffers);
    GST_FBDEVFRAMEBUFFERSINK_MESSAGE_OBJECT(fbdevframebuffersink, s);
    g_free (s);
  }

  return TRUE;

err:
  GST_FBDEVFRAMEBUFFERSINK_MESSAGE_OBJECT (fbdevframebuffersink,
      "Could not initialize fbdev framebuffer device");
  return FALSE;
}

/* The following member function is exported for use by derived subclasses. */
void
gst_fbdevframebuffersink_close_hardware (GstFramebufferSink *framebuffersink)
{
  GstFbdevFramebufferSink *fbdevframebuffersink = GST_FBDEVFRAMEBUFFERSINK (framebuffersink);

  GST_OBJECT_LOCK (fbdevframebuffersink);

  gst_fbdevframebuffersink_pan_display_fbdev(fbdevframebuffersink, 0, 0);

  if (munmap (fbdevframebuffersink->framebuffer, fbdevframebuffersink->framebuffer_map_size))
    GST_ERROR_OBJECT (fbdevframebuffersink, "Could not unmap video memory");

  close (fbdevframebuffersink->fd);

  GST_OBJECT_UNLOCK (fbdevframebuffersink);
}

static void
gst_fbdevframebuffersink_pan_display (GstFramebufferSink *framebuffersink, GstMemory *memory) {
  GstFbdevFramebufferSink *fbdevframebuffersink = GST_FBDEVFRAMEBUFFERSINK (framebuffersink);
  GstMapInfo mapinfo;
  int y;
  gst_memory_map (memory, &mapinfo, 0);
  y = (mapinfo.data - fbdevframebuffersink->framebuffer) /
      fbdevframebuffersink->fixinfo.line_length;
  gst_fbdevframebuffersink_pan_display_fbdev (fbdevframebuffersink, 0, y);
  gst_memory_unmap (memory, &mapinfo);
}

static void
gst_fbdevframebuffersink_wait_for_vsync (GstFramebufferSink *framebuffersink) {
  GstFbdevFramebufferSink *fbdevframebuffersink = GST_FBDEVFRAMEBUFFERSINK (framebuffersink);
  if (ioctl (fbdevframebuffersink->fd, FBIO_WAITFORVSYNC, NULL)) {
    GST_ERROR_OBJECT(fbdevframebuffersink, "FBIO_WAITFORVSYNC call failed. Disabling vsync.");
    framebuffersink->vsync = FALSE;
  }
}

/* Initialize allocation params for the fbdev video memory allocator for either */
/* screens or overlays. */

static void
gst_fbdevframebuffersink_allocation_params_init (GstFbdevFramebufferSink *fbdevframebuffersink,
GstAllocationParams *allocation_params, gboolean is_pannable, gboolean is_overlay)
{
  int i;
  gst_allocation_params_init(allocation_params);
  allocation_params->flags = 0;
  allocation_params->prefix = 0;
  allocation_params->padding = 0;
  if (is_overlay)
    allocation_params->align = fbdevframebuffersink->framebuffersink.overlay_alignment;
  else if (is_pannable) {
    /* Determine the minimum alignment of the framebuffer screen pages. */
    /* The minimum guaranteed alignment is word-aligned (align = 3). */
    for (i = 8; i <= 4096; i <<= 1)
      if (fbdevframebuffersink->fixinfo.line_length & (i - 1))
        break;
    allocation_params->align = (i >> 1) - 1;
  }
  else
    allocation_params->align = 3;
}

static void
gst_fbdevframebuffersink_pan_display_fbdev (GstFbdevFramebufferSink *fbdevframebuffersink,
    int xoffset, int yoffset)
{
  int old_xoffset = fbdevframebuffersink->varinfo.xoffset;
  int old_yoffset = fbdevframebuffersink->varinfo.yoffset;
  fbdevframebuffersink->varinfo.xoffset = xoffset;
  fbdevframebuffersink->varinfo.yoffset = yoffset;
  if (ioctl (fbdevframebuffersink->fd, FBIOPAN_DISPLAY, &fbdevframebuffersink->varinfo)) {
    GST_ERROR_OBJECT (fbdevframebuffersink, "FBIOPAN_DISPLAY call failed");
    fbdevframebuffersink->varinfo.xoffset = old_xoffset;
    fbdevframebuffersink->varinfo.yoffset = old_yoffset;
  }
}

GType
gst_fbdevframebuffersink_get_type (void)
{
  static GType fbdevframebuffersink_type = 0;

  if (!fbdevframebuffersink_type) {
    static const GTypeInfo fbdevframebuffersink_info = {
      sizeof (GstFbdevFramebufferSinkClass),
      gst_fbdevframebuffersink_base_init,
      NULL,
      (GClassInitFunc) gst_fbdevframebuffersink_class_init,
      NULL,
      NULL,
      sizeof (GstFbdevFramebufferSink),
      0,
      (GInstanceInitFunc) gst_fbdevframebuffersink_init,
    };

    fbdevframebuffersink_type = g_type_register_static( GST_TYPE_FRAMEBUFFERSINK,
        "GstFbdevFramebufferSink", &fbdevframebuffersink_info, 0);
  }

  return fbdevframebuffersink_type;
}


/* Video memory implementation for fbdev devices. */

typedef struct
{
  GstMemory mem;
  gpointer data;
} GstFbdevFramebufferSinkVideoMemory;

static gpointer
gst_fbdevframebuffersink_video_memory_map (GstFbdevFramebufferSinkVideoMemory * mem, gsize maxsize, GstMapFlags flags)
{
#if 0
  GST_LOG ("video_memory_map called, mem = %p, maxsize = %d, flags = %d, data = %p\n", mem,
      maxsize, flags, mem->data);

  if (flags & GST_MAP_READ)
    GST_LOG ("Mapping video memory for reading is slow.\n");
#endif

  return mem->data;
}

static gboolean
gst_fbdevframebuffersink_video_memory_unmap (GstFbdevFramebufferSinkVideoMemory * mem)
{
  GST_DEBUG ("%p: unmapped", mem);
  return TRUE;
}

/* Video memory storage. */

typedef struct {
  gpointer framebuffer_address;
  gsize size;
} ChainEntry;

typedef struct {
  GstMiniObject parent;
  gpointer framebuffer;
  gsize framebuffer_size;
  /* The lowest non-allocated offset. */
  gsize end_marker;
  /* The amount of video memory allocated. */
  gsize total_allocated;
  /* Maintain a sorted linked list of allocated memory regions. */
  GList *chain;
} GstFbdevFramebufferSinkVideoMemoryStorage;

GType gst_fbdev_framebuffer_sink_video_memory_storage_get_type (void);
GST_DEFINE_MINI_OBJECT_TYPE (GstFbdevFramebufferSinkVideoMemoryStorage,
    gst_fbdev_framebuffer_sink_video_memory_storage);

static GstFbdevFramebufferSinkVideoMemoryStorage *video_memory_storage;

static void
gst_fbdevframebuffersink_video_memory_init (gpointer framebuffer, gsize framebuffer_size) {
  video_memory_storage = g_slice_new (GstFbdevFramebufferSinkVideoMemoryStorage);
  gst_mini_object_init (GST_MINI_OBJECT_CAST (video_memory_storage),
      GST_MINI_OBJECT_FLAG_LOCKABLE,
      gst_fbdev_framebuffer_sink_video_memory_storage_get_type (),
      NULL, NULL, NULL);
  video_memory_storage->framebuffer = framebuffer;
  video_memory_storage->framebuffer_size = framebuffer_size;
  video_memory_storage->total_allocated = 0;
  video_memory_storage->end_marker = 0;
  video_memory_storage->chain = NULL;
}

/* Video memory allocator implementation that uses fbdev video memory. */

typedef struct
{
  GstAllocator parent;
  GstAllocationParams params;
} GstFbdevFramebufferSinkVideoMemoryAllocator;

typedef struct
{
  GstAllocatorClass parent_class;
} GstFbdevFramebufferSinkVideoMemoryAllocatorClass;

GType gst_fbdevframebuffersink_video_memory_allocator_get_type (void);
G_DEFINE_TYPE (GstFbdevFramebufferSinkVideoMemoryAllocator,
    gst_fbdevframebuffersink_video_memory_allocator, GST_TYPE_ALLOCATOR);

static GstMemory *
gst_fbdevframebuffersink_video_memory_allocator_alloc (GstAllocator *allocator, gsize size,
    GstAllocationParams *allocation_params)
{
  GstFbdevFramebufferSinkVideoMemoryAllocator *fbdevframebuffersink_allocator =
      (GstFbdevFramebufferSinkVideoMemoryAllocator *) allocator;
  GstFbdevFramebufferSinkVideoMemory *mem;
  GstAllocationParams *params;
  int align_bytes;
  guintptr framebuffer_offset;
  GList *chain;
  ChainEntry *chain_entry;

  GST_DEBUG ("alloc frame %u", size);

  gst_mini_object_lock (GST_MINI_OBJECT_CAST (video_memory_storage), GST_LOCK_FLAG_EXCLUSIVE);

  /* Always ignore allocation_params, but use our own specific alignment. */
  params = &fbdevframebuffersink_allocator->params;

  align_bytes = ALIGNMENT_GET_ALIGN_BYTES(video_memory_storage->end_marker, params->align);
  framebuffer_offset = video_memory_storage->end_marker + align_bytes;

  if (video_memory_storage->end_marker + align_bytes + size > video_memory_storage->framebuffer_size) {
      /* When we can't just provide memory from beyond the highest allocated address, */
      /* we to have traverse our chain to find a free spot. */
      ChainEntry *previous_entry;
      chain = video_memory_storage->chain;
      previous_entry = NULL;
      while (chain != NULL) {
        ChainEntry *entry = chain->data;
        gsize gap_size;
        gpointer gap_start;
        if (previous_entry == NULL)
          gap_start = video_memory_storage->framebuffer;
        else
          gap_start = previous_entry->framebuffer_address + previous_entry->size;
        gap_size = entry->framebuffer_address - gap_start;
        align_bytes = ALIGNMENT_GET_ALIGN_BYTES(gap_start - video_memory_storage->framebuffer,
            params->align);
        if (gap_size >= align_bytes + size) {
          /* We found a gap large enough to fit the requested size. */
          framebuffer_offset = gap_start + align_bytes - video_memory_storage->framebuffer;
          break;
        }
        previous_entry = entry;
        chain = g_list_next (chain);
      }
      if (chain == NULL) {
        GST_ERROR ("Out of video memory");
        gst_mini_object_unlock (GST_MINI_OBJECT_CAST (video_memory_storage), GST_LOCK_FLAG_EXCLUSIVE);
        return NULL;
      }
  }

  mem = g_slice_new (GstFbdevFramebufferSinkVideoMemory);

  gst_memory_init (GST_MEMORY_CAST (mem), GST_MEMORY_FLAG_NO_SHARE |
      GST_MEMORY_FLAG_VIDEO_MEMORY, allocator, NULL, size, params->align, 0, size);

  mem->data = video_memory_storage->framebuffer + framebuffer_offset;
  if (framebuffer_offset + size > video_memory_storage->end_marker)
    video_memory_storage->end_marker = framebuffer_offset + size;
  video_memory_storage->total_allocated += size;

  /* Insert the allocated area into the chain. */

  /* Find the first entry whose framebuffer address is greater. */
  chain = video_memory_storage->chain;
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
  video_memory_storage->chain = g_list_insert_before (video_memory_storage->chain,
      chain, chain_entry);

  gst_mini_object_unlock (GST_MINI_OBJECT_CAST (video_memory_storage), GST_LOCK_FLAG_EXCLUSIVE);

  GST_INFO ("Allocated video memory buffer of size %zd at %p, align %zd, mem = %p\n", size,
      mem->data, params->align, mem);

  return (GstMemory *) mem;
}

static void
gst_fbdevframebuffersink_video_memory_allocator_free (GstAllocator * allocator, GstMemory * mem)
{
  GstFbdevFramebufferSinkVideoMemory *vmem = (GstFbdevFramebufferSinkVideoMemory *) mem;
  GList *chain;

  gst_mini_object_lock (GST_MINI_OBJECT_CAST (video_memory_storage), GST_LOCK_FLAG_EXCLUSIVE);

  chain = video_memory_storage->chain;

  while (chain != NULL) {
    ChainEntry *entry = chain->data;
    if (entry->framebuffer_address == vmem->data && entry->size == mem->size) {
      /* Delete this entry. */
      g_slice_free (ChainEntry, entry);
      /* Update the end marker. */
      if (g_list_next (chain) == NULL) {
        GList *previous = g_list_previous (chain);
        if (previous == NULL)
          video_memory_storage->end_marker = 0;
        else {
          ChainEntry *previous_entry = previous->data;
          video_memory_storage->end_marker = previous_entry->framebuffer_address +
              previous_entry->size - video_memory_storage->framebuffer;
        }
      }
      video_memory_storage->chain = g_list_delete_link (video_memory_storage->chain, chain);
      video_memory_storage->total_allocated -= mem->size;
      gst_mini_object_unlock (GST_MINI_OBJECT_CAST (video_memory_storage), GST_LOCK_FLAG_EXCLUSIVE);
      g_slice_free (GstFbdevFramebufferSinkVideoMemory, vmem);
      return;
    }
    chain = g_list_next (chain);
  }

  gst_mini_object_unlock (GST_MINI_OBJECT_CAST (video_memory_storage), GST_LOCK_FLAG_EXCLUSIVE);
  GST_ERROR ("video_memory_free failed");
}

static void
gst_fbdevframebuffersink_video_memory_allocator_class_init (GstFbdevFramebufferSinkVideoMemoryAllocatorClass * klass) {
  GstAllocatorClass * allocator_class = GST_ALLOCATOR_CLASS (klass);

  allocator_class->alloc = gst_fbdevframebuffersink_video_memory_allocator_alloc;
  allocator_class->free = gst_fbdevframebuffersink_video_memory_allocator_free;
}

static void
gst_fbdevframebuffersink_video_memory_allocator_init (GstFbdevFramebufferSinkVideoMemoryAllocator
    *video_memory_allocator) {
  GstAllocator * alloc = GST_ALLOCATOR_CAST (video_memory_allocator);

  alloc->mem_type = "fbdevframebuffersink_video_memory";
  alloc->mem_map = (GstMemoryMapFunction) gst_fbdevframebuffersink_video_memory_map;
  alloc->mem_unmap = (GstMemoryUnmapFunction) gst_fbdevframebuffersink_video_memory_unmap;
}

static GstAllocator *
gst_fbdevframebuffersink_video_memory_allocator_new (GstFramebufferSink *framebuffersink,
GstVideoInfo *info, gboolean pannable, gboolean is_overlay)
{
  GstFbdevFramebufferSink *fbdevframebuffersink = GST_FBDEVFRAMEBUFFERSINK (framebuffersink);
  GstFbdevFramebufferSinkVideoMemoryAllocator *fbdevframebuffersink_video_memory_allocator =
      g_object_new (gst_fbdevframebuffersink_video_memory_allocator_get_type (), NULL);
  char s[80];

  gst_fbdevframebuffersink_allocation_params_init (fbdevframebuffersink,
      &fbdevframebuffersink_video_memory_allocator->params, pannable, is_overlay);

  g_sprintf (s, "fbdevframebuffersink_video_memory_%p", fbdevframebuffersink_video_memory_allocator);
  gst_allocator_register (s, gst_object_ref (fbdevframebuffersink_video_memory_allocator) );

  return GST_ALLOCATOR_CAST (fbdevframebuffersink_video_memory_allocator);
}

