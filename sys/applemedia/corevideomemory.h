/* GStreamer Core Video memory
 * Copyright (C) 2015 Ilya Konstantinov
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

#ifndef __GST_CORE_VIDEO_MEMORY_H__
#define __GST_CORE_VIDEO_MEMORY_H__

#include <gst/gst.h>

#include "CoreVideo/CoreVideo.h"

G_BEGIN_DECLS

typedef enum
{
  GST_CORE_VIDEO_MEMORY_UNLOCKED,
  GST_CORE_VIDEO_MEMORY_LOCKED_READONLY,
  GST_CORE_VIDEO_MEMORY_LOCKED_READ_WRITE
} GstCoreVideoLockState;

/**
 * GstCoreVideoPixelBuffer:
 *
 * References the backing CVPixelBuffer and manages its locking.
 * This shared structure is referenced by all #GstCoreVideoMemory
 * objects (planes and shares) backed by this Core Video pixel buffer.
 */
typedef struct
{
  guint refcount;
  GMutex mutex;
  CVPixelBufferRef buf;
  /* Allows mem_map to refuse Read-Write locking a buffer that was previously
   * locked for Read-Only. */
  GstCoreVideoLockState lock_state;
  /* Counts the number of times the buffer was locked.
   * Only the first lock affects whether its just for reading
   * or for reading and writing, as reflected in @lock_state. */
  guint lock_count;
} GstCoreVideoPixelBuffer;

/**
 * GST_CORE_VIDEO_NO_PLANE:
 *
 * Indicates a non-planar pixel buffer.
 */
#define GST_CORE_VIDEO_NO_PLANE ((size_t)-1)

/**
 * GstCoreVideoMemory:
 *
 * Represents a video plane or an entire (non-planar) video image,
 * backed by a #GstCoreVideoPixelBuffer structure (which wraps
 * a CVPixelBuffer).
 */
typedef struct
{
  GstMemory mem;

  GstCoreVideoPixelBuffer *gpixbuf;
  size_t plane;
} GstCoreVideoMemory;

void
gst_core_video_memory_init (void);

GstCoreVideoPixelBuffer *
gst_core_video_pixel_buffer_create (CVPixelBufferRef pixbuf);

GstCoreVideoPixelBuffer *
gst_core_video_pixel_buffer_ref (GstCoreVideoPixelBuffer * gpixbuf);

void
gst_core_video_pixel_buffer_unref (GstCoreVideoPixelBuffer * gpixbuf);

gboolean
gst_is_core_video_memory (GstMemory * mem);

GstMemory *
gst_core_video_memory_new_wrapped (GstCoreVideoPixelBuffer * gpixbuf, gsize plane, gsize size);

G_END_DECLS

#endif /* __GST_CORE_VIDEO_MEMORY_H__ */
