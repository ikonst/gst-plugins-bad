/*
 * GStreamer
 *
 * unit test for avfvideosrc
 *
 * Copyright (C) 2015 Ilya Konstantinov <ilya.konstantinov@gmail.com>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <gst/check/gstcheck.h>
#include <gst/video/video.h>

static void
got_buf_cb (GstElement * sink, GstBuffer * new_buf, GstPad * pad,
    GstSample ** p_old_sample)
{
  GstCaps *caps;

  caps = gst_pad_get_current_caps (pad);

  if (*p_old_sample)
    gst_sample_unref (*p_old_sample);
  *p_old_sample = gst_sample_new (new_buf, caps, NULL, NULL);

  gst_caps_unref (caps);
}

static void
test_frame_map (GstBuffer * buf, GstCaps * caps, GstMapFlags flags)
{
  GstVideoInfo info;
  GstVideoFrame frame;

  fail_unless (gst_video_info_from_caps (&info, caps));

  fail_unless (gst_video_frame_map (&frame, &info, buf, flags));

  gst_video_frame_unmap (&frame);
}

static void
test_memory (GstBuffer * buffer)
{
  guint n_mem, i;

  n_mem = gst_buffer_n_memory (buffer);
  for (i = 0; i < n_mem; ++i) {
    GstMemory *mem;
    GstMapInfo map;
    gsize size, offset, maxsize;

    fail_unless (mem = gst_buffer_get_memory (buffer, i));

    size = gst_memory_get_sizes (mem, &offset, &maxsize);

    /* test map for GST_MAP_READ */
    fail_unless (gst_memory_map (mem, &map, GST_MAP_READ));

    /* test mem_share */
    {
      GstMemory *mem_share;
      GstMapInfo map_share;

      /* make sure 'share' returns offset to the same locked memory */
      fail_unless (mem_share = gst_memory_share (mem, offset + 1, size - 2));
      fail_unless (gst_memory_map (mem_share, &map_share, GST_MAP_READ));
      fail_unless (map.size == map_share.size + 2);
      fail_unless (map.data == map_share.data - 1);
      gst_memory_unmap (mem_share, &map_share);

      /* make sure 'share' guarantees non-writable memories */
      fail_if (gst_memory_map (mem_share, &map_share, GST_MAP_READWRITE));

      gst_memory_unref (mem_share);
    }

    gst_memory_unmap (mem, &map);

    /* test map for GST_MAP_READWRITE */
    fail_unless (gst_memory_map (mem, &map, GST_MAP_READWRITE));
    gst_memory_unmap (mem, &map);

    gst_memory_unref (mem);
  }
}

static void
test_memory_compatible_maps (GstBuffer * buffer)
{
  GstMemory *mem1, *mem2;
  GstMapInfo map1, map2;

  fail_unless (gst_buffer_n_memory (buffer) >= 2);
  fail_unless (mem1 = gst_buffer_get_memory (buffer, 0));
  fail_unless (mem2 = gst_buffer_get_memory (buffer, 1));

  fail_unless (gst_memory_map (mem1, &map1, GST_MAP_READWRITE));
  fail_unless (gst_memory_map (mem2, &map2, GST_MAP_READ));
  gst_memory_unmap (mem2, &map2);
  gst_memory_unmap (mem1, &map1);

  gst_memory_unref (mem2);
  gst_memory_unref (mem1);
}

static void
test_memory_incompatible_maps (GstBuffer * buffer)
{
  GstMemory *mem1, *mem2;
  GstMapInfo map1, map2;

  fail_unless (gst_buffer_n_memory (buffer) >= 2);
  fail_unless (mem1 = gst_buffer_get_memory (buffer, 0));
  fail_unless (mem2 = gst_buffer_get_memory (buffer, 1));

  fail_unless (gst_memory_map (mem1, &map1, GST_MAP_READ));
  /* at this point, the WHOLE pixbuf is supposed to be locked read-only
   * so the following map should fail */
  fail_if (gst_memory_map (mem2, &map2, GST_MAP_READWRITE));
  gst_memory_unmap (mem1, &map1);

  gst_memory_unref (mem2);
  gst_memory_unref (mem1);
}

static void
test_memory_span (GstBuffer * buffer)
{
  GstMemory *mem1, *mem2;
  gsize offset;

  fail_unless (gst_buffer_n_memory (buffer) >= 2);
  fail_unless (mem1 = gst_buffer_get_memory (buffer, 0));
  fail_unless (mem2 = gst_buffer_get_memory (buffer, 1));

  /* is_span is not implemented; should always return FALSE */
  fail_if (gst_memory_is_span (mem1, mem2, &offset));

  gst_memory_unref (mem2);
  gst_memory_unref (mem1);
}

GST_START_TEST (test_core_video_memory)
{
  GstElement *pipeline, *src, *filter, *sink;
  GstCaps *template_caps;
  GstSample *sample = NULL;
  GstPad *srcpad;

  pipeline = gst_pipeline_new ("pipeline");
  src = gst_check_setup_element ("avfvideosrc");
  filter = gst_check_setup_element ("capsfilter");
  sink = gst_check_setup_element ("fakesink");

  gst_bin_add_many (GST_BIN (pipeline), src, filter, sink, NULL);

  fail_unless (gst_element_link (src, filter));
  fail_unless (gst_element_link (filter, sink));

  srcpad = gst_element_get_static_pad (src, "src");
  template_caps = gst_pad_get_pad_template_caps (srcpad);

  g_object_set (sink, "signal-handoffs", TRUE, NULL);
  g_signal_connect (sink, "preroll-handoff", G_CALLBACK (got_buf_cb), &sample);

  GST_LOG ("avfvideosrc src template caps: %" GST_PTR_FORMAT, template_caps);

  {
    GstCaps *caps;
    GstStateChangeReturn state_ret;

    caps = gst_caps_from_string ("video/x-raw, format=(string)NV12");

    /* get avfvideosrc to produce a buffer with the given caps */
    g_object_set (filter, "caps", caps, NULL);

    state_ret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
    fail_unless (state_ret != GST_STATE_CHANGE_FAILURE,
        "pipeline _set_state() to PLAYING failed");
    state_ret = gst_element_get_state (pipeline, NULL, NULL, -1);
    fail_unless (state_ret == GST_STATE_CHANGE_SUCCESS,
        "pipeline failed going to PLAYING state");

    state_ret = gst_element_set_state (pipeline, GST_STATE_NULL);
    fail_unless (state_ret == GST_STATE_CHANGE_SUCCESS);

    fail_unless (sample != NULL);

    /* process buffer */
    {
      GstBuffer *buf;
      GstStructure *s;
      GstCaps *caps;
      const gchar *format;

      buf = gst_sample_get_buffer (sample);
      fail_unless (buf != NULL);
      caps = gst_sample_get_caps (sample);
      fail_unless (caps != NULL);

      s = gst_caps_get_structure (caps, 0);
      format = gst_structure_get_string (s, "format");

      /* make sure we have planar format NV12 */
      fail_unless (g_str_equal (format, "NV12"));

      /* test GstCoreVideoMemory directly */
      test_memory (buf);
      test_memory_compatible_maps (buf);
      test_memory_incompatible_maps (buf);
      test_memory_span (buf);

      /* test gst_video_frame_map */
      test_frame_map (buf, caps, GST_MAP_READWRITE);
      test_frame_map (buf, caps, GST_MAP_READ);

      gst_sample_unref (sample);
      sample = NULL;
    }

    gst_caps_unref (caps);
  }

  gst_caps_unref (template_caps);
  gst_object_unref (srcpad);

  gst_object_unref (pipeline);
}

GST_END_TEST;

static Suite *
avfvideosrc_suite (void)
{
  Suite *s = suite_create ("avfvideosrc");
  TCase *tc_chain = tcase_create ("general");

  suite_add_tcase (s, tc_chain);
  tcase_add_test (tc_chain, test_core_video_memory);

  return s;
}

GST_CHECK_MAIN (avfvideosrc);
