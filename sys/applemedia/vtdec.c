/* GStreamer
 * Copyright (C) 2010, 2013 Ole André Vadla Ravnås <oleavr@soundrop.com>
 * Copyright (C) 2012, 2013 Alessandro Decina <alessandro.d@gmail.com>
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
 * SECTION:element-gstvtdec
 *
 * Apple VideoToolbox based decoder.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v filesrc location=file.mov ! qtdemux ! queue ! h264parse ! vtdec ! videoconvert ! autovideosink
 * ]|
 * Decode h264 video from a mov file.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideodecoder.h>
#include <gst/gl/gstglcontext.h>
#include "vtdec.h"
#include "vtutil.h"
#include "corevideobuffer.h"
#include "coremediabuffer.h"

GST_DEBUG_CATEGORY_STATIC (gst_vtdec_debug_category);
#define GST_CAT_DEFAULT gst_vtdec_debug_category

static void gst_vtdec_finalize (GObject * object);

static gboolean gst_vtdec_start (GstVideoDecoder * decoder);
static gboolean gst_vtdec_stop (GstVideoDecoder * decoder);
static gboolean gst_vtdec_decide_allocation (GstVideoDecoder * decoder,
    GstQuery * query);
static gboolean gst_vtdec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state);
static gboolean gst_vtdec_flush (GstVideoDecoder * decoder);
static GstFlowReturn gst_vtdec_finish (GstVideoDecoder * decoder);
static GstFlowReturn gst_vtdec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame);

static gboolean gst_vtdec_create_session (GstVtdec * vtdec,
    GstVideoFormat format);
static void gst_vtdec_invalidate_session (GstVtdec * vtdec);
static CMSampleBufferRef cm_sample_buffer_from_gst_buffer (GstVtdec * vtdec,
    GstBuffer * buf);
static GstFlowReturn gst_vtdec_push_frames_if_needed (GstVtdec * vtdec,
    gboolean drain, gboolean flush);
static CMFormatDescriptionRef create_format_description (GstVtdec * vtdec,
    CMVideoCodecType cm_format);
static CMFormatDescriptionRef
create_format_description_from_codec_data (GstVtdec * vtdec,
    CMVideoCodecType cm_format, GstBuffer * codec_data);
static void gst_vtdec_session_output_callback (void
    *decompression_output_ref_con, void *source_frame_ref_con, OSStatus status,
    VTDecodeInfoFlags info_flags, CVImageBufferRef image_buffer, CMTime pts,
    CMTime duration);
static gboolean compute_h264_decode_picture_buffer_length (GstVtdec * vtdec,
    GstBuffer * codec_data, int *length);
static gboolean gst_vtdec_compute_reorder_queue_length (GstVtdec * vtdec,
    CMVideoCodecType cm_format, GstBuffer * codec_data);
static void gst_vtdec_set_latency (GstVtdec * vtdec);

static GstStaticPadTemplate gst_vtdec_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264, stream-format=avc, alignment=au,"
        " width=(int)[1, MAX], height=(int)[1, MAX];"
        "video/mpeg, mpegversion=2;" "image/jpeg")
    );

/* define EnableHardwareAcceleratedVideoDecoder in < 10.9 */
#if defined(MAC_OS_X_VERSION_MAX_ALLOWED) && MAC_OS_X_VERSION_MAX_ALLOWED < 1090
const CFStringRef
    kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder =
CFSTR ("EnableHardwareAcceleratedVideoDecoder");
const CFStringRef
    kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder =
CFSTR ("RequireHardwareAcceleratedVideoDecoder");
#endif

#ifdef HAVE_IOS
#define GST_VTDEC_VIDEO_FORMAT_STR "NV12"
#else
#define GST_VTDEC_VIDEO_FORMAT_STR "UYVY"
#endif

#define VIDEO_SRC_CAPS \
    GST_VIDEO_CAPS_MAKE(GST_VTDEC_VIDEO_FORMAT_STR) ";" \
    GST_VIDEO_CAPS_MAKE_WITH_FEATURES \
    (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, \
        "RGBA") ";"

G_DEFINE_TYPE (GstVtdec, gst_vtdec, GST_TYPE_VIDEO_DECODER);

static void
gst_vtdec_class_init (GstVtdecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstVideoDecoderClass *video_decoder_class = GST_VIDEO_DECODER_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_static_pad_template_get (&gst_vtdec_sink_template));
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
          gst_caps_from_string (VIDEO_SRC_CAPS)));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "Apple VideoToolbox decoder",
      "Codec/Decoder/Video",
      "Apple VideoToolbox Decoder",
      "Ole André Vadla Ravnås <oleavr@soundrop.com>; "
      "Alessandro Decina <alessandro.d@gmail.com>");

  gobject_class->finalize = gst_vtdec_finalize;
  video_decoder_class->start = GST_DEBUG_FUNCPTR (gst_vtdec_start);
  video_decoder_class->stop = GST_DEBUG_FUNCPTR (gst_vtdec_stop);
  video_decoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_vtdec_decide_allocation);
  video_decoder_class->set_format = GST_DEBUG_FUNCPTR (gst_vtdec_set_format);
  video_decoder_class->flush = GST_DEBUG_FUNCPTR (gst_vtdec_flush);
  video_decoder_class->finish = GST_DEBUG_FUNCPTR (gst_vtdec_finish);
  video_decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_vtdec_handle_frame);
}

static void
gst_vtdec_init (GstVtdec * vtdec)
{
  vtdec->reorder_queue = g_async_queue_new ();
}

void
gst_vtdec_finalize (GObject * object)
{
  GstVtdec *vtdec = GST_VTDEC (object);

  GST_DEBUG_OBJECT (vtdec, "finalize");

  g_async_queue_unref (vtdec->reorder_queue);


  G_OBJECT_CLASS (gst_vtdec_parent_class)->finalize (object);
}

static gboolean
gst_vtdec_start (GstVideoDecoder * decoder)
{
  GstVtdec *vtdec = GST_VTDEC (decoder);

  GST_DEBUG_OBJECT (vtdec, "start");

  return TRUE;
}

static gboolean
gst_vtdec_stop (GstVideoDecoder * decoder)
{
  GstVtdec *vtdec = GST_VTDEC (decoder);

  if (vtdec->session)
    gst_vtdec_invalidate_session (vtdec);

  if (vtdec->texture_cache)
    gst_core_video_texture_cache_free (vtdec->texture_cache);
  vtdec->texture_cache = NULL;

  GST_DEBUG_OBJECT (vtdec, "stop");

  return TRUE;
}

static gboolean
gst_vtdec_decide_allocation (GstVideoDecoder * decoder, GstQuery * query)
{
  gboolean ret;
  GstCaps *caps;
  GstCapsFeatures *features;
  GstVtdec *vtdec = GST_VTDEC (decoder);

  ret =
      GST_VIDEO_DECODER_CLASS (gst_vtdec_parent_class)->decide_allocation
      (decoder, query);
  if (!ret)
    goto out;

  gst_query_parse_allocation (query, &caps, NULL);
  if (caps) {
    GstGLContext *gl_context = NULL;
    features = gst_caps_get_features (caps, 0);

    if (gst_caps_features_contains (features,
            GST_CAPS_FEATURE_MEMORY_GL_MEMORY)) {
      GstContext *context = NULL;
      GstQuery *query = gst_query_new_context ("gst.gl.local_context");
      if (gst_pad_peer_query (GST_VIDEO_DECODER_SRC_PAD (decoder), query)) {

        gst_query_parse_context (query, &context);
        if (context) {
          const GstStructure *s = gst_context_get_structure (context);
          gst_structure_get (s, "context", GST_GL_TYPE_CONTEXT, &gl_context,
              NULL);
        }
      }
      gst_query_unref (query);

      if (context) {
        GST_INFO_OBJECT (decoder, "pushing textures. GL context %p", context);
        vtdec->texture_cache = gst_core_video_texture_cache_new (gl_context);
        gst_object_unref (gl_context);
      } else {
        GST_WARNING_OBJECT (decoder,
            "got memory:GLMemory caps but not GL context from downstream element");
      }
    }
  }

out:
  return ret;
}

static GstVideoFormat
gst_vtdec_negotiate_output_format (GstVtdec * vtdec)
{
  GstCaps *caps = NULL;
  GstVideoFormat format;
  GstStructure *structure;
  const gchar *s;

  caps = gst_pad_get_allowed_caps (GST_VIDEO_DECODER_SRC_PAD (vtdec));
  if (!caps)
    caps = gst_pad_query_caps (GST_VIDEO_DECODER_SRC_PAD (vtdec), NULL);
  caps = gst_caps_truncate (caps);
  structure = gst_caps_get_structure (caps, 0);
  s = gst_structure_get_string (structure, "format");
  format = gst_video_format_from_string (s);
  gst_caps_unref (caps);

  return format;
}

static gboolean
gst_vtdec_set_format (GstVideoDecoder * decoder, GstVideoCodecState * state)
{
  GstStructure *structure;
  CMVideoCodecType cm_format = 0;
  CMFormatDescriptionRef format_description = NULL;
  const char *caps_name;
  GstVtdec *vtdec = GST_VTDEC (decoder);
  GstVideoFormat output_format;
  GstVideoCodecState *output_state;

  GST_DEBUG_OBJECT (vtdec, "set_format");

  structure = gst_caps_get_structure (state->caps, 0);
  caps_name = gst_structure_get_name (structure);
  if (!strcmp (caps_name, "video/x-h264")) {
    cm_format = kCMVideoCodecType_H264;
  } else if (!strcmp (caps_name, "video/mpeg")) {
    cm_format = kCMVideoCodecType_MPEG2Video;
  } else if (!strcmp (caps_name, "image/jpeg")) {
    cm_format = kCMVideoCodecType_JPEG;
  }

  if (cm_format == kCMVideoCodecType_H264 && state->codec_data == NULL) {
    GST_INFO_OBJECT (vtdec, "no codec data, wait for one");
    return TRUE;
  }

  if (vtdec->session)
    gst_vtdec_invalidate_session (vtdec);

  gst_video_info_from_caps (&vtdec->video_info, state->caps);

  if (!gst_vtdec_compute_reorder_queue_length (vtdec, cm_format,
          state->codec_data))
    return FALSE;
  gst_vtdec_set_latency (vtdec);

  if (state->codec_data) {
    format_description = create_format_description_from_codec_data (vtdec,
        cm_format, state->codec_data);
  } else {
    format_description = create_format_description (vtdec, cm_format);
  }

  if (vtdec->format_description)
    CFRelease (vtdec->format_description);
  vtdec->format_description = format_description;

  output_format = gst_vtdec_negotiate_output_format (vtdec);
  if (!gst_vtdec_create_session (vtdec, output_format))
    return FALSE;

  output_state = gst_video_decoder_set_output_state (decoder,
      output_format, vtdec->video_info.width, vtdec->video_info.height, state);
  output_state->caps = gst_video_info_to_caps (&output_state->info);
  if (output_state->info.finfo->format == GST_VIDEO_FORMAT_RGBA) {
    gst_caps_set_features (output_state->caps, 0,
        gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, NULL));
  }

  return TRUE;
}

static gboolean
gst_vtdec_flush (GstVideoDecoder * decoder)
{
  GstVtdec *vtdec = GST_VTDEC (decoder);

  GST_DEBUG_OBJECT (vtdec, "flush");

  gst_vtdec_push_frames_if_needed (vtdec, FALSE, TRUE);

  return TRUE;
}

static GstFlowReturn
gst_vtdec_finish (GstVideoDecoder * decoder)
{
  GstVtdec *vtdec = GST_VTDEC (decoder);

  GST_DEBUG_OBJECT (vtdec, "finish");

  return gst_vtdec_push_frames_if_needed (vtdec, TRUE, FALSE);
}

static GstFlowReturn
gst_vtdec_handle_frame (GstVideoDecoder * decoder, GstVideoCodecFrame * frame)
{
  OSStatus status;
  CMSampleBufferRef cm_sample_buffer = NULL;
  VTDecodeFrameFlags input_flags, output_flags;
  GstVtdec *vtdec = GST_VTDEC (decoder);
  GstFlowReturn ret = GST_FLOW_OK;
  int decode_frame_number = frame->decode_frame_number;

  if (vtdec->format_description == NULL) {
    ret = GST_FLOW_NOT_NEGOTIATED;
    goto out;
  }

  GST_LOG_OBJECT (vtdec, "got input frame %d", decode_frame_number);

  ret = gst_vtdec_push_frames_if_needed (vtdec, FALSE, FALSE);
  if (ret != GST_FLOW_OK)
    return ret;

  /* don't bother enabling kVTDecodeFrame_EnableTemporalProcessing at all since
   * it's not mandatory for the underlying VT codec to respect it. KISS and do
   * reordering ourselves.
   */
  input_flags = kVTDecodeFrame_EnableAsynchronousDecompression;
  output_flags = 0;

  cm_sample_buffer =
      cm_sample_buffer_from_gst_buffer (vtdec, frame->input_buffer);
  status =
      VTDecompressionSessionDecodeFrame (vtdec->session, cm_sample_buffer,
      input_flags, frame, NULL);
  if (status != noErr)
    goto error;

  GST_LOG_OBJECT (vtdec, "submitted input frame %d", decode_frame_number);

out:
  if (cm_sample_buffer)
    CFRelease (cm_sample_buffer);
  return ret;

error:
  GST_VIDEO_DECODER_ERROR (vtdec, 1, STREAM, DECODE, (NULL),
      ("VTDecompressionSessionDecodeFrame returned %d", (int) status), ret);
  goto out;
}

static gboolean
gst_vtdec_create_session (GstVtdec * vtdec, GstVideoFormat format)
{
  CFMutableDictionaryRef output_image_buffer_attrs;
  VTDecompressionOutputCallbackRecord callback;
  CFMutableDictionaryRef videoDecoderSpecification;
  OSStatus status;
  guint32 cv_format;

  switch (format) {
    case GST_VIDEO_FORMAT_NV12:
      cv_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
      break;
    case GST_VIDEO_FORMAT_UYVY:
      cv_format = kCVPixelFormatType_422YpCbCr8;
      break;
    case GST_VIDEO_FORMAT_RGBA:
#ifdef HAVE_IOS
      cv_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
#else
      cv_format = kCVPixelFormatType_422YpCbCr8;
#endif
      break;
    default:
      g_warn_if_reached ();
      break;
  }

  videoDecoderSpecification =
      CFDictionaryCreateMutable (NULL, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);

  /* This is the default on iOS and the key does not exist there */
#ifndef HAVE_IOS
  gst_vtutil_dict_set_boolean (videoDecoderSpecification,
      kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder, TRUE);
  if (vtdec->require_hardware)
    gst_vtutil_dict_set_boolean (videoDecoderSpecification,
        kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder,
        TRUE);
#endif

  output_image_buffer_attrs =
      CFDictionaryCreateMutable (NULL, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  gst_vtutil_dict_set_i32 (output_image_buffer_attrs,
      kCVPixelBufferPixelFormatTypeKey, cv_format);
  gst_vtutil_dict_set_i32 (output_image_buffer_attrs, kCVPixelBufferWidthKey,
      vtdec->video_info.width);
  gst_vtutil_dict_set_i32 (output_image_buffer_attrs, kCVPixelBufferHeightKey,
      vtdec->video_info.height);

  callback.decompressionOutputCallback = gst_vtdec_session_output_callback;
  callback.decompressionOutputRefCon = vtdec;

  status = VTDecompressionSessionCreate (NULL, vtdec->format_description,
      videoDecoderSpecification, output_image_buffer_attrs, &callback,
      &vtdec->session);

  CFRelease (output_image_buffer_attrs);

  if (status != noErr) {
    GST_ELEMENT_ERROR (vtdec, RESOURCE, FAILED, (NULL),
        ("VTDecompressionSessionCreate returned %d", (int) status));
    return FALSE;
  }

  return TRUE;
}

static void
gst_vtdec_invalidate_session (GstVtdec * vtdec)
{
  g_return_if_fail (vtdec->session);

  VTDecompressionSessionInvalidate (vtdec->session);
  CFRelease (vtdec->session);
  vtdec->session = NULL;
}

static CMFormatDescriptionRef
create_format_description (GstVtdec * vtdec, CMVideoCodecType cm_format)
{
  OSStatus status;
  CMFormatDescriptionRef format_description;

  status = CMVideoFormatDescriptionCreate (NULL,
      cm_format, vtdec->video_info.width, vtdec->video_info.height,
      NULL, &format_description);
  if (status != noErr)
    return NULL;

  return format_description;
}

static CMFormatDescriptionRef
create_format_description_from_codec_data (GstVtdec * vtdec,
    CMVideoCodecType cm_format, GstBuffer * codec_data)
{
  CMFormatDescriptionRef fmt_desc;
  CFMutableDictionaryRef extensions, par, atoms;
  GstMapInfo map;
  OSStatus status;

  /* Extensions dict */
  extensions =
      CFDictionaryCreateMutable (NULL, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  gst_vtutil_dict_set_string (extensions,
      CFSTR ("CVImageBufferChromaLocationBottomField"), "left");
  gst_vtutil_dict_set_string (extensions,
      CFSTR ("CVImageBufferChromaLocationTopField"), "left");
  gst_vtutil_dict_set_boolean (extensions, CFSTR ("FullRangeVideo"), FALSE);

  /* CVPixelAspectRatio dict */
  par = CFDictionaryCreateMutable (NULL, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  gst_vtutil_dict_set_i32 (par, CFSTR ("HorizontalSpacing"),
      vtdec->video_info.par_n);
  gst_vtutil_dict_set_i32 (par, CFSTR ("VerticalSpacing"),
      vtdec->video_info.par_d);
  gst_vtutil_dict_set_object (extensions, CFSTR ("CVPixelAspectRatio"),
      (CFTypeRef *) par);

  /* SampleDescriptionExtensionAtoms dict */
  gst_buffer_map (codec_data, &map, GST_MAP_READ);
  atoms = CFDictionaryCreateMutable (NULL, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  gst_vtutil_dict_set_data (atoms, CFSTR ("avcC"), map.data, map.size);
  gst_vtutil_dict_set_object (extensions,
      CFSTR ("SampleDescriptionExtensionAtoms"), (CFTypeRef *) atoms);
  gst_buffer_unmap (codec_data, &map);

  status = CMVideoFormatDescriptionCreate (NULL,
      cm_format, vtdec->video_info.width, vtdec->video_info.height,
      extensions, &fmt_desc);

  if (status == noErr)
    return fmt_desc;
  else
    return NULL;
}

/* Custom FreeBlock function for CMBlockBuffer */
static void
cm_block_buffer_freeblock (void *refCon, void *doomedMemoryBlock,
    size_t sizeInBytes)
{
  GstMapInfo *info = (GstMapInfo *) refCon;

  gst_memory_unmap (info->memory, info);
  gst_memory_unref (info->memory);
  g_slice_free (GstMapInfo, info);
}

static CMBlockBufferRef
cm_block_buffer_from_gst_buffer (GstBuffer * buf, GstMapFlags flags)
{
  OSStatus status;
  CMBlockBufferRef bbuf;
  CMBlockBufferCustomBlockSource blockSource;
  guint memcount, i;

  /* Initialize custom block source structure */
  blockSource.version = kCMBlockBufferCustomBlockSourceVersion;
  blockSource.AllocateBlock = NULL;
  blockSource.FreeBlock = cm_block_buffer_freeblock;

  /* Determine number of memory blocks */
  memcount = gst_buffer_n_memory (buf);
  status = CMBlockBufferCreateEmpty (NULL, memcount, 0, &bbuf);
  if (status != kCMBlockBufferNoErr) {
    GST_ERROR ("CMBlockBufferCreateEmpty returned %d", (int) status);
    return NULL;
  }

  /* Go over all GstMemory objects and add them to the CMBlockBuffer */
  for (i = 0; i < memcount; ++i) {
    GstMemory *mem;
    GstMapInfo *info;

    mem = gst_buffer_get_memory (buf, i);

    info = g_slice_new (GstMapInfo);
    if (!gst_memory_map (mem, info, flags)) {
      GST_ERROR ("failed mapping memory");
      g_slice_free (GstMapInfo, info);
      gst_memory_unref (mem);
      CFRelease (bbuf);
      return NULL;
    }

    blockSource.refCon = info;
    status =
        CMBlockBufferAppendMemoryBlock (bbuf, info->data, info->size, NULL,
        &blockSource, 0, info->size, 0);
    if (status != kCMBlockBufferNoErr) {
      GST_ERROR ("CMBlockBufferAppendMemoryBlock returned %d", (int) status);
      gst_memory_unmap (mem, info);
      g_slice_free (GstMapInfo, info);
      gst_memory_unref (mem);
      CFRelease (bbuf);
      return NULL;
    }
  }

  return bbuf;
}

static CMSampleBufferRef
cm_sample_buffer_from_gst_buffer (GstVtdec * vtdec, GstBuffer * buf)
{
  OSStatus status;
  CMBlockBufferRef bbuf = NULL;
  CMSampleBufferRef sbuf = NULL;
  CMSampleTimingInfo sample_timing;
  CMSampleTimingInfo time_array[1];

  g_return_val_if_fail (vtdec->format_description, NULL);

  /* create a block buffer */
  bbuf = cm_block_buffer_from_gst_buffer (buf, GST_MAP_READ);
  if (bbuf == NULL) {
    GST_ELEMENT_ERROR (vtdec, RESOURCE, FAILED, (NULL),
        ("failed creating CMBlockBuffer"));
    return NULL;
  }

  /* create a sample buffer */
  if (GST_BUFFER_DURATION_IS_VALID (buf))
    sample_timing.duration = CMTimeMake (GST_BUFFER_DURATION (buf), GST_SECOND);
  else
    sample_timing.duration = kCMTimeInvalid;

  if (GST_BUFFER_PTS_IS_VALID (buf))
    sample_timing.presentationTimeStamp =
        CMTimeMake (GST_BUFFER_PTS (buf), GST_SECOND);
  else
    sample_timing.presentationTimeStamp = kCMTimeInvalid;

  if (GST_BUFFER_DTS_IS_VALID (buf))
    sample_timing.decodeTimeStamp =
        CMTimeMake (GST_BUFFER_DTS (buf), GST_SECOND);
  else
    sample_timing.decodeTimeStamp = kCMTimeInvalid;

  time_array[0] = sample_timing;

  status =
      CMSampleBufferCreate (NULL, bbuf, TRUE, 0, 0, vtdec->format_description,
      1, 1, time_array, 0, NULL, &sbuf);
  CFRelease (bbuf);
  if (status != noErr) {
    GST_ELEMENT_ERROR (vtdec, RESOURCE, FAILED, (NULL),
        ("CMSampleBufferCreate returned %d", (int) status));
    return NULL;
  }

  return sbuf;
}

static gint
sort_frames_by_pts (gconstpointer f1, gconstpointer f2, gpointer user_data)
{
  GstVideoCodecFrame *frame1, *frame2;
  GstClockTime pts1, pts2;

  frame1 = (GstVideoCodecFrame *) f1;
  frame2 = (GstVideoCodecFrame *) f2;
  pts1 = GST_BUFFER_PTS (frame1->output_buffer);
  pts2 = GST_BUFFER_PTS (frame2->output_buffer);

  if (!GST_CLOCK_TIME_IS_VALID (pts1) || !GST_CLOCK_TIME_IS_VALID (pts2))
    return 0;

  if (pts1 < pts2)
    return -1;
  else if (pts1 == pts2)
    return 0;
  else
    return 1;
}

static void
gst_vtdec_session_output_callback (void *decompression_output_ref_con,
    void *source_frame_ref_con, OSStatus status, VTDecodeInfoFlags info_flags,
    CVImageBufferRef image_buffer, CMTime pts, CMTime duration)
{
  GstVtdec *vtdec = (GstVtdec *) decompression_output_ref_con;
  GstVideoCodecFrame *frame = (GstVideoCodecFrame *) source_frame_ref_con;
  GstBuffer *buf;
  GstVideoCodecState *state;

  GST_LOG_OBJECT (vtdec, "got output frame %p %d and VT buffer %p", frame,
      frame->decode_frame_number, image_buffer);

  if (status != noErr) {
    GST_ERROR_OBJECT (vtdec, "Error decoding frame %d", (int) status);
    goto drop;
  }

  if (image_buffer == NULL) {
    if (info_flags & kVTDecodeInfo_FrameDropped)
      GST_DEBUG_OBJECT (vtdec, "Frame dropped by video toolbox");
    else
      GST_DEBUG_OBJECT (vtdec, "Decoded frame is NULL");
    goto drop;
  }

  /* FIXME: use gst_video_decoder_allocate_output_buffer */
  state = gst_video_decoder_get_output_state (GST_VIDEO_DECODER (vtdec));
  if (state == NULL) {
    GST_WARNING_OBJECT (vtdec, "Output state not configured, release buffer");
    /* release as this usually means that the baseclass isn't ready to do
     * the QoS that _drop requires and will lead to an assertion with the
     * segment.format being undefined */
    goto release;
  }
  buf =
      gst_core_video_buffer_new (image_buffer, &state->info,
      vtdec->texture_cache == NULL);
  gst_video_codec_state_unref (state);

  GST_BUFFER_PTS (buf) = pts.value;
  GST_BUFFER_DURATION (buf) = duration.value;
  frame->output_buffer = buf;
  g_async_queue_push_sorted (vtdec->reorder_queue, frame,
      sort_frames_by_pts, NULL);

  return;

drop:
  GST_WARNING_OBJECT (vtdec, "Frame dropped %p %d", frame,
      frame->decode_frame_number);
  gst_video_decoder_drop_frame (GST_VIDEO_DECODER (vtdec), frame);
  return;

release:
  GST_WARNING_OBJECT (vtdec, "Frame released %p %d", frame,
      frame->decode_frame_number);
  gst_video_decoder_release_frame (GST_VIDEO_DECODER (vtdec), frame);
  return;
}

static GstFlowReturn
gst_vtdec_push_frames_if_needed (GstVtdec * vtdec, gboolean drain,
    gboolean flush)
{
  GstVideoCodecFrame *frame;
  GstFlowReturn ret = GST_FLOW_OK;
  GstVideoDecoder *decoder = GST_VIDEO_DECODER (vtdec);

  /* FIXME: Instead of this, implement GstVideoDecoder::negotiate() and
   * just call gst_video_decoder_negotiate()
   */
  /* negotiate now so that we know whether we need to use the GL upload meta or
   * not */
  if (gst_pad_check_reconfigure (decoder->srcpad)) {
    gst_video_decoder_negotiate (decoder);
    if (vtdec->texture_cache) {
      GstVideoFormat internal_format;
      GstVideoCodecState *output_state =
          gst_video_decoder_get_output_state (decoder);

#ifdef HAVE_IOS
      internal_format = GST_VIDEO_FORMAT_NV12;
#else
      internal_format = GST_VIDEO_FORMAT_UYVY;
#endif
      gst_core_video_texture_cache_set_format (vtdec->texture_cache,
          internal_format, output_state->caps);
      gst_video_codec_state_unref (output_state);
    }
  }

  if (drain)
    VTDecompressionSessionWaitForAsynchronousFrames (vtdec->session);

  /* push a buffer if there are enough frames to guarantee that we push in PTS
   * order
   */
  while ((g_async_queue_length (vtdec->reorder_queue) >=
          vtdec->reorder_queue_length) || drain || flush) {
    frame = (GstVideoCodecFrame *) g_async_queue_try_pop (vtdec->reorder_queue);
    if (frame && vtdec->texture_cache != NULL) {
      frame->output_buffer =
          gst_core_video_texture_cache_get_gl_buffer (vtdec->texture_cache,
          frame->output_buffer);
      if (!frame->output_buffer)
        GST_ERROR_OBJECT (vtdec, "couldn't get textures from buffer");
    }

    /* we need to check this in case reorder_queue_length=0 (jpeg for
     * example) or we're draining/flushing
     */
    if (frame) {
      if (flush)
        gst_video_decoder_drop_frame (decoder, frame);
      else
        ret = gst_video_decoder_finish_frame (decoder, frame);
    }

    if (!frame || ret != GST_FLOW_OK)
      break;
  }

  return ret;
}

static gboolean
parse_h264_profile_and_level_from_codec_data (GstVtdec * vtdec,
    GstBuffer * codec_data, int *profile, int *level)
{
  GstMapInfo map;
  guint8 *data;
  gint size;
  gboolean ret = TRUE;

  gst_buffer_map (codec_data, &map, GST_MAP_READ);
  data = map.data;
  size = map.size;

  /* parse the avcC data */
  if (size < 7)
    goto avcc_too_small;

  /* parse the version, this must be 1 */
  if (data[0] != 1)
    goto wrong_version;

  /* AVCProfileIndication */
  /* profile_compat */
  /* AVCLevelIndication */
  if (profile)
    *profile = data[1];

  if (level)
    *level = data[3];

out:
  gst_buffer_unmap (codec_data, &map);

  return ret;

avcc_too_small:
  GST_ELEMENT_ERROR (vtdec, STREAM, DECODE, (NULL),
      ("invalid codec_data buffer length"));
  ret = FALSE;
  goto out;

wrong_version:
  GST_ELEMENT_ERROR (vtdec, STREAM, DECODE, (NULL),
      ("wrong avcC version in codec_data"));
  ret = FALSE;
  goto out;
}

static int
get_dpb_max_mb_s_from_level (GstVtdec * vtdec, int level)
{
  switch (level) {
    case 10:
      /* 1b?? */
      return 396;
    case 11:
      return 900;
    case 12:
    case 13:
    case 20:
      return 2376;
    case 21:
      return 4752;
    case 22:
    case 30:
      return 8100;
    case 31:
      return 18000;
    case 32:
      return 20480;
    case 40:
    case 41:
      return 32768;
    case 42:
      return 34816;
    case 50:
      return 110400;
    case 51:
    case 52:
      return 184320;
    default:
      GST_ERROR_OBJECT (vtdec, "unknown level %d", level);
      return -1;
  }
}

static gboolean
gst_vtdec_compute_reorder_queue_length (GstVtdec * vtdec,
    CMVideoCodecType cm_format, GstBuffer * codec_data)
{
  if (cm_format == kCMVideoCodecType_H264) {
    if (!compute_h264_decode_picture_buffer_length (vtdec, codec_data,
            &vtdec->reorder_queue_length)) {
      return FALSE;
    }
  } else {
    vtdec->reorder_queue_length = 0;
  }

  return TRUE;
}

static gboolean
compute_h264_decode_picture_buffer_length (GstVtdec * vtdec,
    GstBuffer * codec_data, int *length)
{
  int profile, level;
  int dpb_mb_size = 16;
  int max_dpb_size_frames = 16;
  int max_dpb_mb_s = -1;
  int width_in_mb_s = GST_ROUND_UP_16 (vtdec->video_info.width) / dpb_mb_size;
  int height_in_mb_s = GST_ROUND_UP_16 (vtdec->video_info.height) / dpb_mb_size;

  *length = 0;

  if (!parse_h264_profile_and_level_from_codec_data (vtdec, codec_data,
          &profile, &level))
    return FALSE;

  if (vtdec->video_info.width == 0 || vtdec->video_info.height == 0)
    return FALSE;

  GST_INFO_OBJECT (vtdec, "parsed profile %d, level %d", profile, level);
  if (profile == 66) {
    /* baseline or constrained-baseline, we don't need to reorder */
    return TRUE;
  }

  max_dpb_mb_s = get_dpb_max_mb_s_from_level (vtdec, level);
  if (max_dpb_mb_s == -1) {
    GST_ELEMENT_ERROR (vtdec, STREAM, DECODE, (NULL),
        ("invalid level in codec_data, could not compute max_dpb_mb_s"));
    return FALSE;
  }

  /* this formula is specified in sections A.3.1.h and A.3.2.f of the 2009
   * edition of the standard */
  *length = MIN (floor (max_dpb_mb_s / (width_in_mb_s * height_in_mb_s)),
      max_dpb_size_frames);
  return TRUE;
}

static void
gst_vtdec_set_latency (GstVtdec * vtdec)
{
  GstClockTime frame_duration;
  GstClockTime latency;

  if (vtdec->video_info.fps_n == 0) {
    GST_INFO_OBJECT (vtdec, "Framerate not known, can't set latency");
    return;
  }

  frame_duration = gst_util_uint64_scale (GST_SECOND,
      vtdec->video_info.fps_d, vtdec->video_info.fps_n);
  latency = frame_duration * vtdec->reorder_queue_length;

  GST_INFO_OBJECT (vtdec, "setting latency frames:%d time:%" GST_TIME_FORMAT,
      vtdec->reorder_queue_length, GST_TIME_ARGS (latency));
  gst_video_decoder_set_latency (GST_VIDEO_DECODER (vtdec), latency, latency);
}

#ifndef HAVE_IOS
#define GST_TYPE_VTDEC_HW   (gst_vtdec_hw_get_type())
#define GST_VTDEC_HW(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_VTDEC_HW,GstVtdecHw))
#define GST_VTDEC_HW_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_VTDEC_HW,GstVtdecHwClass))
#define GST_IS_VTDEC_HW(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_VTDEC_HW))
#define GST_IS_VTDEC_HW_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_VTDEC_HW))

typedef GstVtdec GstVtdecHw;
typedef GstVtdecClass GstVtdecHwClass;

GType gst_vtdec_hw_get_type (void);

G_DEFINE_TYPE (GstVtdecHw, gst_vtdec_hw, GST_TYPE_VTDEC);

static void
gst_vtdec_hw_class_init (GstVtdecHwClass * klass)
{
  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "Apple VideoToolbox decoder (hardware only)",
      "Codec/Decoder/Video",
      "Apple VideoToolbox Decoder",
      "Ole André Vadla Ravnås <oleavr@soundrop.com>; "
      "Alessandro Decina <alessandro.d@gmail.com>");
}

static void
gst_vtdec_hw_init (GstVtdecHw * vtdec)
{
  GST_VTDEC (vtdec)->require_hardware = TRUE;
}

#endif

void
gst_vtdec_register_elements (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_vtdec_debug_category, "vtdec", 0,
      "debug category for vtdec element");

#ifdef HAVE_IOS
  gst_element_register (plugin, "vtdec", GST_RANK_PRIMARY, GST_TYPE_VTDEC);
#else
  gst_element_register (plugin, "vtdec_hw", GST_RANK_PRIMARY + 1,
      GST_TYPE_VTDEC_HW);
  gst_element_register (plugin, "vtdec", GST_RANK_SECONDARY, GST_TYPE_VTDEC);
#endif
}
