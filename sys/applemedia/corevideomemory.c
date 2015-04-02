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
 * Library General Public License for mordetails.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "corevideomemory.h"

GST_DEBUG_CATEGORY_STATIC (GST_CAT_CORE_VIDEO_MEMORY);
#define GST_CAT_DEFAULT GST_CAT_CORE_VIDEO_MEMORY

static const char *_lock_state_names[] = {
  "Unlocked", "Locked Read-Only", "Locked Read-Write"
};

/**
 * gst_core_video_pixel_buffer_create:
 *
 * Initializes an empty wrapper for a CVPixelBuffer. It is expected
 * that the CVPixelBuffer was not, and will not be locked independently
 * but only through this structure, for the duration of its existence.
 */
GstCoreVideoPixelBuffer *
gst_core_video_pixel_buffer_create (CVPixelBufferRef pixbuf)
{
  GstCoreVideoPixelBuffer *gpixbuf = g_slice_new (GstCoreVideoPixelBuffer);
  gpixbuf->refcount = 1;
  g_mutex_init (&gpixbuf->mutex);
  gpixbuf->buf = CVPixelBufferRetain (pixbuf);
  gpixbuf->lock_state = GST_CORE_VIDEO_MEMORY_UNLOCKED;
  gpixbuf->lock_count = 0;
  return gpixbuf;
}

GstCoreVideoPixelBuffer *
gst_core_video_pixel_buffer_ref (GstCoreVideoPixelBuffer * gpixbuf)
{
  g_atomic_int_inc (&gpixbuf->refcount);
  return gpixbuf;
}

void
gst_core_video_pixel_buffer_unref (GstCoreVideoPixelBuffer * gpixbuf)
{
  if (g_atomic_int_dec_and_test (&gpixbuf->refcount)) {
    if (gpixbuf->lock_state != GST_CORE_VIDEO_MEMORY_UNLOCKED)
      GST_ERROR
          ("%p: CVPixelBuffer memory still locked (lock_count = %d), likely forgot to unmap GstCoreVideoMemory",
          gpixbuf, gpixbuf->lock_count);
    CVPixelBufferRelease (gpixbuf->buf);
    g_mutex_clear (&gpixbuf->mutex);
    g_slice_free (GstCoreVideoPixelBuffer, gpixbuf);
  }
}

/**
 * gst_core_video_pixel_buffer_lock:
 *
 * Locks the pixel buffer into CPU memory for reading or reading
 * and writing (as indicated by @flags). Only the first lock affects the mode;
 * subsequent calls only ensure the requested locking mode is the same
 * as currently in effect.
 *
 * This function can be called multiple time by the same or different
 * referencing GstCoreVideoMemory objects; care should be taken to call
 * gst_core_video_pixel_buffer_unlock() for every call to this function.
 */
static gboolean
gst_core_video_pixel_buffer_lock (GstCoreVideoPixelBuffer * gpixbuf,
    GstMapFlags flags)
{
  CVReturn cvret;
  CVOptionFlags lockFlags;

  g_mutex_lock (&gpixbuf->mutex);

  switch (gpixbuf->lock_state) {
    case GST_CORE_VIDEO_MEMORY_UNLOCKED:
      lockFlags = (flags & GST_MAP_WRITE) ? 0 : kCVPixelBufferLock_ReadOnly;
      cvret = CVPixelBufferLockBaseAddress (gpixbuf->buf, lockFlags);
      if (cvret != kCVReturnSuccess) {
        g_mutex_unlock (&gpixbuf->mutex);
        /* TODO: Map kCVReturnError etc. into strings */
        GST_ERROR ("%p: unable to lock base address for pixbuf %p: %d", gpixbuf,
            gpixbuf->buf, cvret);
        return FALSE;
      }
      gpixbuf->lock_state =
          (flags & GST_MAP_WRITE) ? GST_CORE_VIDEO_MEMORY_LOCKED_READ_WRITE :
          GST_CORE_VIDEO_MEMORY_LOCKED_READONLY;
      break;

    case GST_CORE_VIDEO_MEMORY_LOCKED_READONLY:
      if (flags & GST_MAP_WRITE) {
        g_mutex_unlock (&gpixbuf->mutex);
        GST_ERROR ("%p: pixel buffer %p already locked for read-only access",
            gpixbuf, gpixbuf->buf);
        return FALSE;
      }
      break;

    case GST_CORE_VIDEO_MEMORY_LOCKED_READ_WRITE:
      break;                    /* nothing to do, already most permissive mapping */
  }

  g_atomic_int_inc (&gpixbuf->lock_count);

  g_mutex_unlock (&gpixbuf->mutex);

  GST_DEBUG ("%p: pixbuf %p, %s (%d times)",
      gpixbuf,
      gpixbuf->buf,
      _lock_state_names[gpixbuf->lock_state], gpixbuf->lock_count);

  return TRUE;
}

/**
 * gst_core_video_pixel_buffer_unlock:
 *
 * Unlocks the pixel buffer from CPU memory. Should be called
 * for every gst_core_video_pixel_buffer_lock() call.
 */
static gboolean
gst_core_video_pixel_buffer_unlock (GstCoreVideoPixelBuffer * gpixbuf)
{
  CVOptionFlags lockFlags;
  CVReturn cvret;

  if (gpixbuf->lock_state == GST_CORE_VIDEO_MEMORY_UNLOCKED) {
    GST_ERROR ("%p: pixel buffer %p not locked", gpixbuf, gpixbuf->buf);
    return FALSE;
  }

  if (!g_atomic_int_dec_and_test (&gpixbuf->lock_count)) {
    return TRUE;                /* still locked, by current and/or other callers */
  }

  g_mutex_lock (&gpixbuf->mutex);

  lockFlags =
      (gpixbuf->lock_state ==
      GST_CORE_VIDEO_MEMORY_LOCKED_READONLY) ? kCVPixelBufferLock_ReadOnly : 0;
  cvret = CVPixelBufferUnlockBaseAddress (gpixbuf->buf, lockFlags);
  if (cvret != kCVReturnSuccess) {
    g_mutex_unlock (&gpixbuf->mutex);
    g_atomic_int_inc (&gpixbuf->lock_count);
    /* TODO: Map kCVReturnError etc. into strings */
    GST_ERROR ("%p: unable to unlock base address for pixbuf %p: %d", gpixbuf,
        gpixbuf->buf, cvret);
    return FALSE;
  }

  gpixbuf->lock_state = GST_CORE_VIDEO_MEMORY_UNLOCKED;

  g_mutex_unlock (&gpixbuf->mutex);

  GST_DEBUG ("%p: pixbuf %p, %s (%d locks remaining)",
      gpixbuf,
      gpixbuf->buf,
      _lock_state_names[gpixbuf->lock_state], gpixbuf->lock_count);

  return TRUE;
}

/*
 * GstCoreVideoAllocator
 */

typedef GstAllocator GstCoreVideoAllocator;
typedef GstAllocatorClass GstCoreVideoAllocatorClass;
GType gst_core_video_allocator_get_type (void);
#define GST_TYPE_CORE_VIDEO_ALLOCATOR             (gst_core_video_allocator_get_type())
#define GST_IS_CORE_VIDEO_ALLOCATOR(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_CORE_VIDEO_ALLOCATOR))
#define GST_IS_CORE_VIDEO_ALLOCATOR_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_CORE_VIDEO_ALLOCATOR))
#define GST_CORE_VIDEO_ALLOCATOR_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_CORE_VIDEO_ALLOCATOR, GstCoreVideoAllocatorClass))
#define GST_CORE_VIDEO_ALLOCATOR(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_CORE_VIDEO_ALLOCATOR, GstCoreVideoAllocator))
#define GST_CORE_VIDEO_ALLOCATOR_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_CORE_VIDEO_ALLOCATOR, GstCoreVideoAllocatorClass))
#define GST_CORE_VIDEO_ALLOCATOR_CAST(obj)        ((GsCoreVideoAllocator *)(obj))
#define GST_CORE_VIDEO_ALLOCATOR_NAME "CoreVideoMemory"

/* Singleton instance of GstCoreVideoAllocator */
static GstCoreVideoAllocator *_core_video_allocator;

G_DEFINE_TYPE (GstCoreVideoAllocator, gst_core_video_allocator,
    GST_TYPE_ALLOCATOR);

/**
 * gst_core_video_memory_init:
 *
 * Initializes the Core Video Memory allocator. It is safe to call this function
 * multiple times. This must be called before any other #GstCoreVideoMemory operation.
 */
void
gst_core_video_memory_init (void)
{
  static volatile gsize _init = 0;

  if (g_once_init_enter (&_init)) {
    GST_DEBUG_CATEGORY_INIT (GST_CAT_CORE_VIDEO_MEMORY, "corevideomemory", 0,
        "Core Video Memory");

    _core_video_allocator =
        g_object_new (gst_core_video_allocator_get_type (), NULL);

    gst_allocator_register (GST_CORE_VIDEO_ALLOCATOR_NAME,
        gst_object_ref (_core_video_allocator));
    g_once_init_leave (&_init, 1);
  }
}

/**
 * gst_is_core_video_memory:
 *
 * Indicates whether this GstMemory is backed by a CVPixelBuffer.
 */
gboolean
gst_is_core_video_memory (GstMemory * mem)
{
  return mem != NULL && GST_IS_CORE_VIDEO_ALLOCATOR (mem->allocator);
}

/**
 * gst_core_video_memory_new:
 *
 * Helper function for gst_core_video_mem_share().
 * Users should call gst_core_video_memory_new_wrapped() instead.
 */
static GstMemory *
gst_core_video_memory_new (GstMemoryFlags flags, GstMemory * parent,
    GstCoreVideoPixelBuffer * gpixbuf, gsize plane, gsize maxsize, gsize align,
    gsize offset, gsize size)
{
  GstCoreVideoMemory *mem;

  g_return_val_if_fail (gpixbuf != NULL, NULL);

  mem = g_slice_new0 (GstCoreVideoMemory);
  gst_memory_init (GST_MEMORY_CAST (mem), flags,
      GST_ALLOCATOR_CAST (_core_video_allocator), parent, maxsize, align,
      offset, size);

  mem->gpixbuf = gst_core_video_pixel_buffer_ref (gpixbuf);
  mem->plane = plane;

  GST_DEBUG ("%p: gpixbuf %p, plane: %" G_GSSIZE_FORMAT ", size %"
      G_GSIZE_FORMAT, mem, mem->gpixbuf, mem->plane, mem->mem.size);

  return (GstMemory *) mem;
}

/**
 * gst_core_video_memory_new_wrapped:
 * @gpixbuf: the backing #GstCoreVideoPixelBuffer
 * @plane: the plane this memory will represent, or #GST_CORE_VIDEO_NO_PLANE for non-planar buffer
 * @size: the size of the buffer or specific plane
 *
 * Returns: a newly allocated #GstCoreVideoMemory
 */
GstMemory *
gst_core_video_memory_new_wrapped (GstCoreVideoPixelBuffer * gpixbuf,
    gsize plane, gsize size)
{
  return gst_core_video_memory_new (0, NULL, gpixbuf, plane, size, 0, 0, size);
}

static gpointer
gst_core_video_mem_map (GstMemory * gmem, gsize maxsize, GstMapFlags flags)
{
  GstCoreVideoMemory *mem = (GstCoreVideoMemory *) gmem;
  gpointer ret;

  if (!gst_core_video_pixel_buffer_lock (mem->gpixbuf, flags))
    return NULL;

  if (mem->plane != GST_CORE_VIDEO_NO_PLANE) {
    ret = CVPixelBufferGetBaseAddressOfPlane (mem->gpixbuf->buf, mem->plane);

    if (ret != NULL)
      GST_DEBUG ("%p: pixbuf %p plane %" G_GSIZE_FORMAT
          " flags %08x: mapped %p", mem, mem->gpixbuf->buf, mem->plane, flags,
          ret);
    else
      GST_ERROR ("%p: invalid plane base address (NULL) for pixbuf %p plane %"
          G_GSIZE_FORMAT, mem, mem->gpixbuf->buf, mem->plane);
  } else {
    ret = CVPixelBufferGetBaseAddress (mem->gpixbuf->buf);

    if (ret != NULL)
      GST_DEBUG ("%p: pixbuf %p flags %08x: mapped %p", mem, mem->gpixbuf->buf,
          flags, ret);
    else
      GST_ERROR ("%p: invalid base address (NULL) for pixbuf %p"
          G_GSIZE_FORMAT, mem, mem->gpixbuf->buf);
  }

  return ret;
}

static void
gst_core_video_mem_unmap (GstMemory * gmem)
{
  GstCoreVideoMemory *mem = (GstCoreVideoMemory *) gmem;
  (void) gst_core_video_pixel_buffer_unlock (mem->gpixbuf);
  if (mem->plane != GST_CORE_VIDEO_NO_PLANE)
    GST_DEBUG ("%p: pixbuf %p plane %" G_GSIZE_FORMAT, mem,
        mem->gpixbuf->buf, mem->plane);
  else
    GST_DEBUG ("%p: pixbuf %p", mem, mem->gpixbuf->buf);
}

static GstMemory *
gst_core_video_mem_share (GstMemory * gmem, gssize offset, gssize size)
{
  GstCoreVideoMemory *mem;
  GstMemory *parent, *sub;

  mem = (GstCoreVideoMemory *) gmem;

  /* find the real parent */
  parent = gmem->parent;
  if (parent == NULL)
    parent = gmem;

  if (size == -1)
    size = gmem->size - offset;

  /* the shared memory is always readonly */
  sub =
      gst_core_video_memory_new (GST_MINI_OBJECT_FLAGS (parent) |
      GST_MINI_OBJECT_FLAG_LOCK_READONLY, parent, mem->gpixbuf, mem->plane,
      gmem->maxsize, gmem->align, gmem->offset + offset, size);

  return sub;
}

static gboolean
gst_core_video_mem_is_span (GstMemory * mem1, GstMemory * mem2, gsize * offset)
{
  /* We may only return FALSE since:
   * 1) Core Video gives no guarantees about planes being consecutive.
   *    We may only know this after mapping.
   * 2) GstCoreVideoMemory instances for planes do not share a common
   *    parent -- i.e. they're not offsets into the same parent
   *    memory instance.
   *
   * It's not unlikely that planes will be stored in consecutive memory
   * but it should be checked by the user after mapping.
   */
  return FALSE;
}

static void
gst_core_video_mem_free (GstAllocator * allocator, GstMemory * gmem)
{
  GstCoreVideoMemory *mem = (GstCoreVideoMemory *) gmem;

  gst_core_video_pixel_buffer_unref (mem->gpixbuf);

  g_slice_free (GstCoreVideoMemory, mem);
}

static void
gst_core_video_allocator_class_init (GstCoreVideoAllocatorClass * klass)
{
  GstAllocatorClass *allocator_class;

  allocator_class = (GstAllocatorClass *) klass;

  /* we don't do allocations, only wrap existing pixel buffers */
  allocator_class->alloc = NULL;
  allocator_class->free = gst_core_video_mem_free;
}

static void
gst_core_video_allocator_init (GstCoreVideoAllocator * allocator)
{
  GstAllocator *alloc = GST_ALLOCATOR_CAST (allocator);

  alloc->mem_type = GST_CORE_VIDEO_ALLOCATOR_NAME;
  alloc->mem_map = gst_core_video_mem_map;
  alloc->mem_unmap = gst_core_video_mem_unmap;
  alloc->mem_share = gst_core_video_mem_share;
  alloc->mem_is_span = gst_core_video_mem_is_span;

  GST_OBJECT_FLAG_SET (allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}
