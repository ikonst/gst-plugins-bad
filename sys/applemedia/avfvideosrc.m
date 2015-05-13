/*
 * Copyright (C) 2010 Ole André Vadla Ravnås <oleavr@soundrop.com>
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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "avfvideosrc.h"

#import <AVFoundation/AVFoundation.h>
#if !HAVE_IOS
#import <AppKit/AppKit.h>
#endif
#include <gst/video/video.h>
#include <gst/gl/gstglcontext.h>
#include <gst/applemedia/coremediabuffer.h>
#include "corevideotexturecache.h"

#define DEFAULT_DEVICE_INDEX  -1
#define DEFAULT_DO_STATS      FALSE

#define DEVICE_FPS_N          25
#define DEVICE_FPS_D          1

#define BUFFER_QUEUE_SIZE     2

GST_DEBUG_CATEGORY (gst_avf_video_src_debug);
#define GST_CAT_DEFAULT gst_avf_video_src_debug

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, "
        "format = (string) { NV12, UYVY, YUY2 }, "
        "framerate = " GST_VIDEO_FPS_RANGE ", "
        "width = " GST_VIDEO_SIZE_RANGE ", "
        "height = " GST_VIDEO_SIZE_RANGE "; "

        "video/x-raw, "
        "format = (string) BGRA, "
        "framerate = " GST_VIDEO_FPS_RANGE ", "
        "width = " GST_VIDEO_SIZE_RANGE ", "
        "height = " GST_VIDEO_SIZE_RANGE "; "

        GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_GL_MEMORY,
            "RGBA") "; "
));

typedef enum _QueueState {
  NO_BUFFERS = 1,
  HAS_BUFFER_OR_STOP_REQUEST,
} QueueState;

#define gst_avf_video_src_parent_class parent_class
G_DEFINE_TYPE (GstAVFVideoSrc, gst_avf_video_src, GST_TYPE_PUSH_SRC);

@interface GstAVFVideoSrcImpl : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
  GstElement *element;
  GstBaseSrc *baseSrc;
  GstPushSrc *pushSrc;

  AVCaptureSession *externalSession;
  gint deviceIndex;
  BOOL doStats;

  AVCaptureSession *session;
  AVCaptureInput *input;
  AVCaptureVideoDataOutput *output;
  AVCaptureDevice *device;
  AVCaptureConnection *connection;
  CMClockRef inputClock;

  dispatch_queue_t mainQueue;
  dispatch_queue_t workerQueue;
  NSConditionLock *bufQueueLock;
  NSMutableArray *bufQueue;
  BOOL stopRequest;
  BOOL disableDelegate;

  GstCaps *caps;
  GstVideoFormat internalFormat;
  GstVideoFormat format;
  gint width, height;
  GstClockTime latency;
  guint64 offset;

  GstClockTime lastSampling;
  guint count;
  gint fps;
  AVCaptureVideoOrientation orientation;
  BOOL captureScreen;
  BOOL captureScreenCursor;
  BOOL captureScreenMouseClicks;

  BOOL useVideoMeta;
  GstCoreVideoTextureCache *textureCache;
}

- (id)init;
- (id)initWithSrc:(GstPushSrc *)src;
- (void)finalize;

@property (readonly) AVCaptureSession *session;
@property (retain) AVCaptureSession *externalSession;
@property int deviceIndex;
@property BOOL doStats;
@property int fps;
@property AVCaptureVideoOrientation orientation;
@property BOOL captureScreen;
@property BOOL captureScreenCursor;
@property BOOL captureScreenMouseClicks;

- (BOOL)openScreenInput;
- (BOOL)openDeviceInput;
- (BOOL)findExternalDeviceInput;
- (BOOL)openDevice;
- (void)closeDevice;
- (GstVideoFormat)getGstVideoFormat:(NSNumber *)pixel_format;
#if !HAVE_IOS
- (CGDirectDisplayID)getDisplayIdFromDeviceIndex;
#endif
- (BOOL)getDeviceCaps:(GstCaps *)result;
- (BOOL)setDeviceCaps:(GstVideoInfo *)info;
- (BOOL)getSessionPresetCaps:(GstCaps *)result;
- (BOOL)setSessionPresetCaps:(GstVideoInfo *)info;
- (GstCaps *)getCaps;
- (BOOL)setCaps:(GstCaps *)new_caps;
- (BOOL)start;
- (BOOL)stop;
- (BOOL)unlock;
- (BOOL)unlockStop;
- (BOOL)query:(GstQuery *)query;
- (GstStateChangeReturn)changeState:(GstStateChange)transition;
- (GstFlowReturn)create:(GstBuffer **)buf;
- (void)updateStatistics;
- (void)captureOutput:(AVCaptureOutput *)captureOutput
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection *)connection;

@end

@implementation GstAVFVideoSrcImpl

@synthesize
  session,
  externalSession,
  deviceIndex,
  doStats,
  fps,
  orientation,
  captureScreen,
  captureScreenCursor,
  captureScreenMouseClicks;

- (id)init
{
  return [self initWithSrc:NULL];
}

- (id)initWithSrc:(GstPushSrc *)src
{
  if ((self = [super init])) {
    element = GST_ELEMENT_CAST (src);
    baseSrc = GST_BASE_SRC_CAST (src);
    pushSrc = src;

    externalSession = nil;
    deviceIndex = DEFAULT_DEVICE_INDEX;
    orientation = AVCaptureVideoOrientationLandscapeRight;
    captureScreen = NO;
    captureScreenCursor = NO;
    captureScreenMouseClicks = NO;
    useVideoMeta = NO;
    textureCache = NULL;

    mainQueue =
        dispatch_queue_create ("org.freedesktop.gstreamer.avfvideosrc.main", NULL);
    workerQueue =
        dispatch_queue_create ("org.freedesktop.gstreamer.avfvideosrc.output", NULL);

    gst_base_src_set_live (baseSrc, TRUE);
    gst_base_src_set_format (baseSrc, GST_FORMAT_TIME);
  }

  return self;
}

- (void)finalize
{
  dispatch_release (mainQueue);
  mainQueue = NULL;
  dispatch_release (workerQueue);
  workerQueue = NULL;

  /* avoid triggering property observers */
  [self->externalSession release];

  [super finalize];
}

- (BOOL)openDeviceInput
{
  NSString *mediaType = AVMediaTypeVideo;
  NSError *err;

  if (deviceIndex == DEFAULT_DEVICE_INDEX) {
    device = [AVCaptureDevice defaultDeviceWithMediaType:mediaType];
    if (device == nil) {
      GST_ELEMENT_ERROR (element, RESOURCE, NOT_FOUND,
                          ("No video capture devices found"), (NULL));
      return NO;
    }
  } else {
    NSArray *devices = [AVCaptureDevice devicesWithMediaType:mediaType];
    if (deviceIndex >= [devices count]) {
      GST_ELEMENT_ERROR (element, RESOURCE, NOT_FOUND,
                          ("Invalid video capture device index"), (NULL));
      return NO;
    }
    device = [devices objectAtIndex:deviceIndex];
  }
  g_assert (device != nil);
  [device retain];

  GST_INFO ("Opening '%s'", [[device localizedName] UTF8String]);

  input = [AVCaptureDeviceInput deviceInputWithDevice:device
                                                error:&err];
  if (input == nil) {
    GST_ELEMENT_ERROR (element, RESOURCE, BUSY,
        ("Failed to open device: %s",
        [[err localizedDescription] UTF8String]),
        (NULL));
    [device release];
    device = nil;
    return NO;
  }
  [input retain];
  return YES;
}

- (BOOL)openScreenInput
{
#if HAVE_IOS
  return NO;
#else
  CGDirectDisplayID displayId;

  GST_DEBUG_OBJECT (element, "Opening screen input");

  displayId = [self getDisplayIdFromDeviceIndex];
  if (displayId == 0)
    return NO;

  AVCaptureScreenInput *screenInput =
      [[AVCaptureScreenInput alloc] initWithDisplayID:displayId];


  @try {
    [screenInput setValue:[NSNumber numberWithBool:captureScreenCursor]
                 forKey:@"capturesCursor"];

  } @catch (NSException *exception) {
    if (![[exception name] isEqualToString:NSUndefinedKeyException]) {
      GST_WARNING ("An unexpected error occured: %s",
                   [[exception reason] UTF8String]);
    }
    GST_WARNING ("Capturing cursor is only supported in OS X >= 10.8");
  }
  screenInput.capturesMouseClicks = captureScreenMouseClicks;
  input = screenInput;
  [input retain];
  device = nil;
  return YES;
#endif
}

- (BOOL)findExternalDeviceInput
{
  NSArray *inputs = [externalSession inputs];

  input = [[inputs firstObject] retain];
  if (input == nil) {
    GST_ERROR_OBJECT (element, "External session has no inputs.");
    return NO;
  }

  if ([inputs count] > 1)
    GST_WARNING_OBJECT (element, "External session has more than one input; taking the first input.");

  if ([input isKindOfClass:[AVCaptureDeviceInput class]])
    device = [[(AVCaptureDeviceInput*)input device] retain];
  else
    device = nil;

  return YES;
}

- (BOOL)openDevice
{
  BOOL success = NO, *successPtr = &success;

  GST_DEBUG_OBJECT (element, "Opening device");

  dispatch_sync (mainQueue, ^{
    BOOL ret;

    if (externalSession == nil) {
      if (captureScreen)
        ret = [self openScreenInput];
      else
        ret = [self openDeviceInput];
    } else {
      ret = [self findExternalDeviceInput];
    }

    if (!ret)
      return;

    output = [[AVCaptureVideoDataOutput alloc] init];
    output.alwaysDiscardsLateVideoFrames = YES;
    output.videoSettings = nil; /* device native format */

    if (externalSession == nil) {
      session = [[AVCaptureSession alloc] init];
      [session addInput:input];
    } else {
      session = [externalSession retain];
    }
    [session addOutput:output];

    /* retained by session */
    connection = [[output connections] firstObject];
    inputClock = ((AVCaptureInputPort *)connection.inputPorts[0]).clock;

    /* rotates the image buffer physically */
    [connection setVideoOrientation:orientation];

    *successPtr = YES;
  });

  GST_DEBUG_OBJECT (element, "Opening device %s", success ? "succeed" : "failed");

  return success;
}

- (void)closeDevice
{
  GST_DEBUG_OBJECT (element, "Closing device");

  dispatch_sync (mainQueue, ^{
    if (externalSession == nil) {
      g_assert (![session isRunning]);
      [session removeInput:input];
    }
    [session removeOutput:output];

    connection = nil;
    inputClock = nil;

    [session release];
    session = nil;

    [output release];
    output = nil;

    [input release];
    input = nil;

    if (device != nil) {
      [device release];
      device = nil;
    }

    if (caps)
      gst_caps_unref (caps);
    caps = NULL;
  });
}


#define GST_AVF_CAPS_NEW(format, w, h, fps_n, fps_d)                  \
    (gst_caps_new_simple ("video/x-raw",                              \
        "width", G_TYPE_INT, w,                                       \
        "height", G_TYPE_INT, h,                                      \
        "format", G_TYPE_STRING, gst_video_format_to_string (format), \
        "framerate", GST_TYPE_FRACTION, (fps_n), (fps_d),             \
        NULL))

- (GstVideoFormat)getGstVideoFormat:(NSNumber *)pixel_format
{
  GstVideoFormat gst_format = GST_VIDEO_FORMAT_UNKNOWN;

  switch ([pixel_format integerValue]) {
  case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange: /* 420v */
    gst_format = GST_VIDEO_FORMAT_NV12;
    break;
  case kCVPixelFormatType_422YpCbCr8: /* 2vuy */
    gst_format = GST_VIDEO_FORMAT_UYVY;
    break;
  case kCVPixelFormatType_32BGRA: /* BGRA */
    gst_format = GST_VIDEO_FORMAT_BGRA;
    break;
  case kCVPixelFormatType_32RGBA: /* RGBA */
    gst_format = GST_VIDEO_FORMAT_RGBA;
    break;
  case kCVPixelFormatType_422YpCbCr8_yuvs: /* yuvs */
    gst_format = GST_VIDEO_FORMAT_YUY2;
    break;
  default:
    GST_LOG_OBJECT (element, "Pixel format %s is not handled by avfvideosrc",
        [[pixel_format stringValue] UTF8String]);
    break;
  }

  return gst_format;
}

#if !HAVE_IOS
- (CGDirectDisplayID)getDisplayIdFromDeviceIndex
{
  NSDictionary *description;
  NSNumber *displayId;
  NSArray *screens = [NSScreen screens];

  if (deviceIndex == DEFAULT_DEVICE_INDEX)
    return kCGDirectMainDisplay;
  if (deviceIndex >= [screens count]) {
    GST_ELEMENT_ERROR (element, RESOURCE, NOT_FOUND,
                        ("Invalid screen capture device index"), (NULL));
    return 0;
  }
  description = [[screens objectAtIndex:deviceIndex] deviceDescription];
  displayId = [description objectForKey:@"NSScreenNumber"];
  return [displayId unsignedIntegerValue];
}
#endif

- (BOOL)getDeviceCaps:(GstCaps *)result
{
  NSArray *formats = [device valueForKey:@"formats"];
  NSArray *pixel_formats = output.availableVideoCVPixelFormatTypes;

  GST_DEBUG_OBJECT (element, "Getting device caps");

  /* Do not use AVCaptureDeviceFormat or AVFrameRateRange only
   * available in iOS >= 7.0. We use a dynamic approach with key-value
   * coding or performSelector */
  for (NSObject *f in [formats reverseObjectEnumerator]) {
    CMFormatDescriptionRef formatDescription;
    CMVideoDimensions dimensions;

    /* formatDescription can't be retrieved with valueForKey so use a selector here */
    formatDescription = (CMFormatDescriptionRef) [f performSelector:@selector(formatDescription)];
    dimensions = CMVideoFormatDescriptionGetDimensions(formatDescription);
    if ([self swapWidthHeight]) {
      int32_t tmp = dimensions.width;
      dimensions.width = dimensions.height;
      dimensions.height = tmp;
    }
    for (NSObject *rate in [f valueForKey:@"videoSupportedFrameRateRanges"]) {
      int fps_n, fps_d;
      gdouble max_fps;

      [[rate valueForKey:@"maxFrameRate"] getValue:&max_fps];
      gst_util_double_to_fraction (max_fps, &fps_n, &fps_d);

      for (NSNumber *pixel_format in pixel_formats) {
        GstVideoFormat gst_format = [self getGstVideoFormat:pixel_format];
        if (gst_format != GST_VIDEO_FORMAT_UNKNOWN)
          gst_caps_append (result, GST_AVF_CAPS_NEW (gst_format, dimensions.width, dimensions.height, fps_n, fps_d));

        if (gst_format == GST_VIDEO_FORMAT_BGRA) {
          GstCaps *rgba_caps = GST_AVF_CAPS_NEW (GST_VIDEO_FORMAT_RGBA, dimensions.width, dimensions.height, fps_n, fps_d);
          gst_caps_set_features (rgba_caps, 0, gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, NULL));
          gst_caps_append (result, rgba_caps);
        }
      }
    }
  }
  GST_LOG_OBJECT (element, "Device returned the following caps %" GST_PTR_FORMAT, result);
  return YES;
}

- (BOOL)setDeviceCaps:(GstVideoInfo *)info
{
  double framerate;
  gboolean found_format = FALSE, found_framerate = FALSE;
  NSArray *formats = [device valueForKey:@"formats"];
  gst_util_fraction_to_double (info->fps_n, info->fps_d, &framerate);

  GST_DEBUG_OBJECT (element, "Setting device caps");

  if ([device lockForConfiguration:NULL] == YES) {
    for (NSObject *f in formats) {
      CMFormatDescriptionRef formatDescription;
      CMVideoDimensions dimensions;

      formatDescription = (CMFormatDescriptionRef) [f performSelector:@selector(formatDescription)];
      dimensions = CMVideoFormatDescriptionGetDimensions(formatDescription);
      if ([self swapWidthHeight]) {
        int32_t tmp = dimensions.width;
        dimensions.width = dimensions.height;
        dimensions.height = tmp;
      }
      if (dimensions.width == info->width && dimensions.height == info->height) {
        found_format = TRUE;
        [device setValue:f forKey:@"activeFormat"];
        for (NSObject *rate in [f valueForKey:@"videoSupportedFrameRateRanges"]) {
          gdouble max_frame_rate;

          [[rate valueForKey:@"maxFrameRate"] getValue:&max_frame_rate];
          if (fabs (framerate - max_frame_rate) < 0.00001) {
            NSValue *min_frame_duration, *max_frame_duration;

            found_framerate = TRUE;
            min_frame_duration = [rate valueForKey:@"minFrameDuration"];
            max_frame_duration = [rate valueForKey:@"maxFrameDuration"];
            [device setValue:min_frame_duration forKey:@"activeVideoMinFrameDuration"];
            @try {
              /* Only available on OSX >= 10.8 and iOS >= 7.0 */
              // Restrict activeVideoMaxFrameDuration to the minimum value so we get a better capture frame rate
              [device setValue:min_frame_duration forKey:@"activeVideoMaxFrameDuration"];
            } @catch (NSException *exception) {
              if (![[exception name] isEqualToString:NSUndefinedKeyException]) {
                GST_WARNING ("An unexcepted error occured: %s",
                              [exception.reason UTF8String]);
              }
            }
            break;
          }
        }
      }
    }
    if (!found_format) {
      GST_WARNING ("Unsupported capture dimensions %dx%d", info->width, info->height);
      return NO;
    }
    if (!found_framerate) {
      GST_WARNING ("Unsupported capture framerate %d/%d", info->fps_n, info->fps_d);
      return NO;
    }
  } else {
    GST_WARNING ("Couldn't lock device for configuration");
    return NO;
  }
  return YES;
}

- (void)updateConnectionOrientation
{
  GST_OBJECT_LOCK (element);
  if (connection != nil)
    [connection setVideoOrientation:orientation];
  GST_OBJECT_UNLOCK (element);
}

- (BOOL)swapWidthHeight
{
  return orientation == AVCaptureVideoOrientationPortrait ||
         orientation == AVCaptureVideoOrientationPortraitUpsideDown;
}

- (void)appendCapsTo:(GstCaps *)result format:(GstVideoFormat)gst_format width:(unsigned int)aWidth height:(unsigned int)aHeight
{
  if ([self swapWidthHeight])
    gst_caps_append (result, GST_AVF_CAPS_NEW (gst_format, aHeight, aWidth, DEVICE_FPS_N, DEVICE_FPS_D));
  else
    gst_caps_append (result, GST_AVF_CAPS_NEW (gst_format, aWidth, aHeight, DEVICE_FPS_N, DEVICE_FPS_D));
}

- (BOOL)getSessionPresetCaps:(GstCaps *)result
{
  NSArray *pixel_formats = output.availableVideoCVPixelFormatTypes;
  for (NSNumber *pixel_format in pixel_formats) {
    GstVideoFormat gst_format = [self getGstVideoFormat:pixel_format];
    if (gst_format == GST_VIDEO_FORMAT_UNKNOWN)
      continue;

#if HAVE_IOS
    if ([session canSetSessionPreset:AVCaptureSessionPreset1920x1080])
      [self appendCapsTo:result format:gst_format width:1920 height:1080];
#endif
    if ([session canSetSessionPreset:AVCaptureSessionPreset1280x720])
      [self appendCapsTo:result format:gst_format width:1280 height:720];
    if ([session canSetSessionPreset:AVCaptureSessionPreset640x480])
      [self appendCapsTo:result format:gst_format width:640 height:480];
    if ([session canSetSessionPreset:AVCaptureSessionPresetMedium])
      [self appendCapsTo:result format:gst_format width:480 height:360];
    if ([session canSetSessionPreset:AVCaptureSessionPreset352x288])
      [self appendCapsTo:result format:gst_format width:352 height:288];
    if ([session canSetSessionPreset:AVCaptureSessionPresetLow])
      [self appendCapsTo:result format:gst_format width:192 height:144];
  }

  GST_LOG_OBJECT (element, "Session presets returned the following caps %" GST_PTR_FORMAT, result);

  return YES;
}

- (BOOL)setSessionPresetCaps:(GstVideoInfo *)info;
{
  GST_DEBUG_OBJECT (element, "Setting session presset caps");

  if ([device lockForConfiguration:NULL] != YES) {
    GST_WARNING ("Couldn't lock device for configuration");
    return NO;
  }

  switch (info->width) {
  case 192:
    session.sessionPreset = AVCaptureSessionPresetLow;
    break;
  case 352:
    session.sessionPreset = AVCaptureSessionPreset352x288;
    break;
  case 480:
    session.sessionPreset = AVCaptureSessionPresetMedium;
    break;
  case 640:
    session.sessionPreset = AVCaptureSessionPreset640x480;
    break;
  case 1280:
    session.sessionPreset = AVCaptureSessionPreset1280x720;
    break;
#if HAVE_IOS
  case 1920:
    session.sessionPreset = AVCaptureSessionPreset1920x1080;
    break;
#endif
  default:
    GST_WARNING ("Unsupported capture dimensions %dx%d", info->width, info->height);
    return NO;
  }
  return YES;
}

- (GstCaps *)getCaps
{
  GstCaps *result;
  NSArray *pixel_formats;

  if (session == nil)
    return NULL; /* BaseSrc will return template caps */

  result = gst_caps_new_empty ();
  pixel_formats = output.availableVideoCVPixelFormatTypes;

  if (captureScreen) {
#if !HAVE_IOS
    CGRect rect = CGDisplayBounds ([self getDisplayIdFromDeviceIndex]);
    for (NSNumber *pixel_format in pixel_formats) {
      GstVideoFormat gst_format = [self getGstVideoFormat:pixel_format];
      if (gst_format != GST_VIDEO_FORMAT_UNKNOWN)
        gst_caps_append (result, gst_caps_new_simple ("video/x-raw",
            "width", G_TYPE_INT, (int)rect.size.width,
            "height", G_TYPE_INT, (int)rect.size.height,
            "format", G_TYPE_STRING, gst_video_format_to_string (gst_format),
            NULL));
    }
#else
    GST_WARNING ("Screen capture is not supported by iOS");
#endif
    return result;
  }

  @try {

    [self getDeviceCaps:result];

  } @catch (NSException *exception) {

    if (![[exception name] isEqualToString:NSUndefinedKeyException]) {
      GST_WARNING ("An unexcepted error occured: %s", [exception.reason UTF8String]);
      return result;
    }

    /* Fallback on session presets API for iOS < 7.0 */
    [self getSessionPresetCaps:result];
  }

  return result;
}

- (BOOL)setCaps:(GstCaps *)new_caps
{
  GstVideoInfo info;
  BOOL success = YES, *successPtr = &success;

  gst_video_info_init (&info);
  gst_video_info_from_caps (&info, new_caps);

  width = info.width;
  height = info.height;
  format = info.finfo->format;
  internalFormat = GST_VIDEO_FORMAT_UNKNOWN;
  latency = gst_util_uint64_scale (GST_SECOND, info.fps_d, info.fps_n);

  if (externalSession != nil) {
    if (caps)
      gst_caps_unref (caps);
    caps = gst_caps_copy (new_caps);
    return YES;
  }

  dispatch_sync (mainQueue, ^{
    int newformat;

    g_assert (![session isRunning]);

    if (captureScreen) {
#if !HAVE_IOS
      AVCaptureScreenInput *screenInput = (AVCaptureScreenInput *)input;
      screenInput.minFrameDuration = CMTimeMake(info.fps_d, info.fps_n);
#else
      GST_WARNING ("Screen capture is not supported by iOS");
      *successPtr = NO;
      return;
#endif
    } else {
      @try {

        /* formats and activeFormat keys are only available on OSX >= 10.7 and iOS >= 7.0 */
        *successPtr = [self setDeviceCaps:(GstVideoInfo *)&info];
        if (*successPtr != YES)
          return;

      } @catch (NSException *exception) {

        if (![[exception name] isEqualToString:NSUndefinedKeyException]) {
          GST_WARNING ("An unexcepted error occured: %s", [exception.reason UTF8String]);
          *successPtr = NO;
          return;
        }

        /* Fallback on session presets API for iOS < 7.0 */
        *successPtr = [self setSessionPresetCaps:(GstVideoInfo *)&info];
        if (*successPtr != YES)
          return;
      }
    }

    internalFormat = format;
    switch (format) {
      case GST_VIDEO_FORMAT_NV12:
        newformat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        break;
      case GST_VIDEO_FORMAT_UYVY:
        newformat = kCVPixelFormatType_422YpCbCr8;
        break;
      case GST_VIDEO_FORMAT_YUY2:
        newformat = kCVPixelFormatType_422YpCbCr8_yuvs;
        break;
      case GST_VIDEO_FORMAT_RGBA:
#if !HAVE_IOS
        newformat = kCVPixelFormatType_422YpCbCr8;
        internalFormat = GST_VIDEO_FORMAT_UYVY;
#else
        newformat = kCVPixelFormatType_32BGRA;
        internalFormat = GST_VIDEO_FORMAT_BGRA;
#endif
        break;
      case GST_VIDEO_FORMAT_BGRA:
        newformat = kCVPixelFormatType_32BGRA;
        break;
      default:
        *successPtr = NO;
        GST_WARNING ("Unsupported output format %s",
            gst_video_format_to_string (format));
        return;
    }

    GST_INFO_OBJECT(element,
        "width: %d height: %d format: %s internalFormat: %s", width, height,
        gst_video_format_to_string (format),
        gst_video_format_to_string (internalFormat));

    output.videoSettings = [NSDictionary
        dictionaryWithObject:[NSNumber numberWithInt:newformat]
        forKey:(NSString*)kCVPixelBufferPixelFormatTypeKey];

    if (caps)
      gst_caps_unref (caps);
    caps = gst_caps_copy (new_caps);

    [session startRunning];

    /* Unlock device configuration only after session is started so the session
     * won't reset the capture formats */
    [device unlockForConfiguration];
  });

  return success;
}

- (BOOL)start
{
  bufQueueLock = [[NSConditionLock alloc] initWithCondition:NO_BUFFERS];
  bufQueue = [[NSMutableArray alloc] initWithCapacity:BUFFER_QUEUE_SIZE];
  stopRequest = NO;

  offset = 0;
  latency = GST_CLOCK_TIME_NONE;

  lastSampling = GST_CLOCK_TIME_NONE;
  count = 0;
  fps = -1;

  /* setting the delegate only after initialization is done */
  [output setSampleBufferDelegate:self
                            queue:workerQueue];
  disableDelegate = NO;

  return YES;
}

- (BOOL)stop
{
  if (externalSession == nil) {
    dispatch_sync (mainQueue, ^{ [session stopRunning]; });
  }

  /* remove the delegate and ensure any pending delegates are no-op */
  [output setSampleBufferDelegate:nil queue:NULL];
  [bufQueueLock lock];
  disableDelegate = YES;
  [bufQueueLock unlockWithCondition:HAS_BUFFER_OR_STOP_REQUEST];

  [bufQueueLock release];
  bufQueueLock = nil;
  [bufQueue release];
  bufQueue = nil;

  if (textureCache)
      gst_core_video_texture_cache_free (textureCache);
  textureCache = NULL;

  return YES;
}

- (BOOL)query:(GstQuery *)query
{
  BOOL result = NO;

  if (GST_QUERY_TYPE (query) == GST_QUERY_LATENCY) {
    if (device != nil && caps != NULL) {
      GstClockTime min_latency, max_latency;

      min_latency = max_latency = latency;
      result = YES;

      GST_DEBUG_OBJECT (element, "reporting latency of min %" GST_TIME_FORMAT
          " max %" GST_TIME_FORMAT,
          GST_TIME_ARGS (min_latency), GST_TIME_ARGS (max_latency));
      gst_query_set_latency (query, TRUE, min_latency, max_latency);
    }
  } else {
    result = GST_BASE_SRC_CLASS (parent_class)->query (baseSrc, query);
  }

  return result;
}

- (BOOL)decideAllocation:(GstQuery *)query
{
  useVideoMeta = gst_query_find_allocation_meta (query,
      GST_VIDEO_META_API_TYPE, NULL);

  /* determine whether we can pass GL textures to downstream element */
  GstCapsFeatures *features = gst_caps_get_features (caps, 0);
  if (gst_caps_features_contains (features, GST_CAPS_FEATURE_MEMORY_GL_MEMORY)) {
    GstGLContext *glContext = NULL;

    /* get GL context from downstream element */
    GstQuery *query = gst_query_new_context ("gst.gl.local_context");
    if (gst_pad_peer_query (GST_BASE_SRC_PAD (element), query)) {
      GstContext *context;
      gst_query_parse_context (query, &context);
      if (context) {
        const GstStructure *s = gst_context_get_structure (context);
        gst_structure_get (s, "context", GST_GL_TYPE_CONTEXT, &glContext,
            NULL);
      }
    }
    gst_query_unref (query);

    if (glContext) {
      GST_INFO_OBJECT (element, "pushing textures. Internal format %s, context %p",
          gst_video_format_to_string (internalFormat), glContext);
      textureCache = gst_core_video_texture_cache_new (glContext);
      gst_core_video_texture_cache_set_format (textureCache, internalFormat, caps);
      gst_object_unref (glContext);
    } else {
      GST_WARNING_OBJECT (element, "got memory:GLMemory caps but not GL context from downstream element");
    }
  } 

  return YES;
}

- (BOOL)unlock
{
  [bufQueueLock lock];
  stopRequest = YES;
  [bufQueueLock unlockWithCondition:HAS_BUFFER_OR_STOP_REQUEST];

  return YES;
}

- (BOOL)unlockStop
{
  [bufQueueLock lock];
  stopRequest = NO;
  [bufQueueLock unlockWithCondition:([bufQueue count] == 0) ? NO_BUFFERS : HAS_BUFFER_OR_STOP_REQUEST];

  return YES;
}

- (GstStateChangeReturn)changeState:(GstStateChange)transition
{
  GstStateChangeReturn ret;

  if (transition == GST_STATE_CHANGE_NULL_TO_READY) {
    if (![self openDevice])
      return GST_STATE_CHANGE_FAILURE;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  if (transition == GST_STATE_CHANGE_READY_TO_NULL)
    [self closeDevice];

  return ret;
}

- (void)captureOutput:(AVCaptureOutput *)captureOutput
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection *)aConnection
{
  GstClockTime timestamp, duration;

  [bufQueueLock lock];

  if (stopRequest || disableDelegate) {
    [bufQueueLock unlock];
    return;
  }

  [self getSampleBuffer:sampleBuffer timestamp:&timestamp duration:&duration];

  if (timestamp == GST_CLOCK_TIME_NONE) {
    [bufQueueLock unlockWithCondition:([bufQueue count] == 0) ? NO_BUFFERS : HAS_BUFFER_OR_STOP_REQUEST];
    return;
  }

  if ([bufQueue count] == BUFFER_QUEUE_SIZE)
    [bufQueue removeLastObject];

  [bufQueue insertObject:@{@"sbuf": (id)sampleBuffer,
                           @"timestamp": @(timestamp),
                           @"duration": @(duration)}
                 atIndex:0];

  [bufQueueLock unlockWithCondition:HAS_BUFFER_OR_STOP_REQUEST];
}

- (GstFlowReturn)create:(GstBuffer **)buf
{
  CMSampleBufferRef sbuf;
  CVImageBufferRef image_buf;
  CVPixelBufferRef pixel_buf;
  size_t cur_width, cur_height;
  GstClockTime timestamp, duration;

  [bufQueueLock lockWhenCondition:HAS_BUFFER_OR_STOP_REQUEST];
  if (stopRequest) {
    [bufQueueLock unlock];
    return GST_FLOW_FLUSHING;
  }

  NSDictionary *dic = (NSDictionary *) [bufQueue lastObject];
  sbuf = (CMSampleBufferRef) dic[@"sbuf"];
  timestamp = (GstClockTime) [dic[@"timestamp"] longLongValue];
  duration = (GstClockTime) [dic[@"duration"] longLongValue];
  CFRetain (sbuf);
  [bufQueue removeLastObject];
  [bufQueueLock unlockWithCondition:
      ([bufQueue count] == 0) ? NO_BUFFERS : HAS_BUFFER_OR_STOP_REQUEST];

  /* Check output frame size dimensions */
  image_buf = CMSampleBufferGetImageBuffer (sbuf);
  if (image_buf) {
    pixel_buf = (CVPixelBufferRef) image_buf;
    cur_width = CVPixelBufferGetWidth (pixel_buf);
    cur_height = CVPixelBufferGetHeight (pixel_buf);

    if (width != cur_width || height != cur_height) {
      /* Set new caps according to current frame dimensions */
      GST_WARNING ("Output frame size has changed %dx%d -> %dx%d, updating caps",
          width, height, (int)cur_width, (int)cur_height);
      width = cur_width;
      height = cur_height;
      gst_caps_set_simple (caps,
        "width", G_TYPE_INT, width,
        "height", G_TYPE_INT, height,
        NULL);
      gst_pad_push_event (GST_BASE_SINK_PAD (baseSrc), gst_event_new_caps (caps));
    }
  }

  *buf = gst_core_media_buffer_new (sbuf, useVideoMeta, textureCache == NULL);
  if (*buf == NULL) {
    CFRelease (sbuf);
    return GST_FLOW_ERROR;
  }

  if (format == GST_VIDEO_FORMAT_RGBA) {
    /* So now buf contains BGRA data (!) . Since downstream is actually going to
     * use the GL upload meta to get RGBA textures (??), we need to override the
     * VideoMeta format (!!!). Yes this is confusing, see setCaps:  */
    GstVideoMeta *video_meta = gst_buffer_get_video_meta (*buf);
    if (video_meta) {
      video_meta->format = format;
    }
  }
  CFRelease (sbuf);

  if (textureCache != NULL) {
    *buf = gst_core_video_texture_cache_get_gl_buffer (textureCache, *buf);
    if (*buf == NULL)
      return GST_FLOW_ERROR;
  }

  GST_BUFFER_OFFSET (*buf) = offset++;
  GST_BUFFER_OFFSET_END (*buf) = GST_BUFFER_OFFSET (*buf) + 1;
  GST_BUFFER_TIMESTAMP (*buf) = timestamp;
  GST_BUFFER_DURATION (*buf) = duration;

  if (doStats)
    [self updateStatistics];

  return GST_FLOW_OK;
}

- (void)getSampleBuffer:(CMSampleBufferRef)sbuf
              timestamp:(GstClockTime *)outTimestamp
               duration:(GstClockTime *)outDuration
{
  CMSampleTimingInfo time_info;
  GstClockTime timestamp, avf_timestamp, duration, input_clock_now, input_clock_diff, running_time;
  CMItemCount num_timings;
  GstClock *clock;
  CMTime now;

  timestamp = GST_CLOCK_TIME_NONE;
  duration = GST_CLOCK_TIME_NONE;
  if (CMSampleBufferGetOutputSampleTimingInfoArray(sbuf, 1, &time_info, &num_timings) == noErr) {
    avf_timestamp = gst_util_uint64_scale (GST_SECOND,
            time_info.presentationTimeStamp.value, time_info.presentationTimeStamp.timescale);

    if (CMTIME_IS_VALID (time_info.duration) && time_info.duration.timescale != 0)
      duration = gst_util_uint64_scale (GST_SECOND,
          time_info.duration.value, time_info.duration.timescale);

    now = CMClockGetTime(inputClock);
    input_clock_now = gst_util_uint64_scale (GST_SECOND,
        now.value, now.timescale);
    input_clock_diff = input_clock_now - avf_timestamp;

    GST_OBJECT_LOCK (element);
    clock = GST_ELEMENT_CLOCK (element);
    if (clock) {
      running_time = gst_clock_get_time (clock) - element->base_time;
      /* We use presentationTimeStamp to determine how much time it took
       * between capturing and receiving the frame in our delegate
       * (e.g. how long it spent in AVF queues), then we subtract that time
       * from our running time to get the actual timestamp.
       */
      if (running_time >= input_clock_diff)
        timestamp = running_time - input_clock_diff;
      else
        timestamp = running_time;

      GST_DEBUG_OBJECT (element, "AVF clock: %"GST_TIME_FORMAT ", AVF PTS: %"GST_TIME_FORMAT
          ", AVF clock diff: %"GST_TIME_FORMAT
          ", running time: %"GST_TIME_FORMAT ", out PTS: %"GST_TIME_FORMAT,
          GST_TIME_ARGS (input_clock_now), GST_TIME_ARGS (avf_timestamp),
          GST_TIME_ARGS (input_clock_diff),
          GST_TIME_ARGS (running_time), GST_TIME_ARGS (timestamp));
    } else {
      /* no clock, can't set timestamps */
      timestamp = GST_CLOCK_TIME_NONE;
    }
    GST_OBJECT_UNLOCK (element);
  }

  *outTimestamp = timestamp;
  *outDuration = duration;
}

- (void)updateStatistics
{
  GstClock *clock;

  GST_OBJECT_LOCK (element);
  clock = GST_ELEMENT_CLOCK (element);
  if (clock != NULL)
    gst_object_ref (clock);
  GST_OBJECT_UNLOCK (element);

  if (clock != NULL) {
    GstClockTime now = gst_clock_get_time (clock);
    gst_object_unref (clock);

    count++;

    if (GST_CLOCK_TIME_IS_VALID (lastSampling)) {
      if (now - lastSampling >= GST_SECOND) {
        GST_OBJECT_LOCK (element);
        fps = count;
        GST_OBJECT_UNLOCK (element);

        g_object_notify (G_OBJECT (element), "fps");

        lastSampling = now;
        count = 0;
      }
    } else {
      lastSampling = now;
    }
  }
}

@end

/*
 * Glue code
 */

enum
{
  PROP_0,
  PROP_SESSION,
  PROP_DEVICE_INDEX,
  PROP_DO_STATS,
  PROP_FPS,
  PROP_ORIENTATION,
#if !HAVE_IOS
  PROP_CAPTURE_SCREEN,
  PROP_CAPTURE_SCREEN_CURSOR,
  PROP_CAPTURE_SCREEN_MOUSE_CLICKS,
#endif
};

#define GST_TYPE_AVFVIDEOSRC_ORIENTATION (gst_avfvideosrc_orientation_get_type ())
static GType
gst_avfvideosrc_orientation_get_type (void)
{
  static GType avfvideosrc_orientation_type = 0;

  if (!avfvideosrc_orientation_type) {
    static GEnumValue orientation_types[] = {
      { AVCaptureVideoOrientationPortrait, "AVCaptureVideoOrientationPortrait", "portrait" },
      { AVCaptureVideoOrientationPortraitUpsideDown, "AVCaptureVideoOrientationPortraitUpsideDown", "portrait-upside-down"  },
      { AVCaptureVideoOrientationLandscapeRight, "AVCaptureVideoOrientationLandscapeRight", "landscape-right" },
      { AVCaptureVideoOrientationLandscapeLeft, "AVCaptureVideoOrientationLandscapeLeft", "landscape-left" },
      { 0, NULL, NULL },
    };

    avfvideosrc_orientation_type =
	g_enum_register_static ("GstAvfvideosrcOrientation",
				orientation_types);
  }

  return avfvideosrc_orientation_type;
}

static void gst_avf_video_src_finalize (GObject * obj);
static void gst_avf_video_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_avf_video_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static GstStateChangeReturn gst_avf_video_src_change_state (
    GstElement * element, GstStateChange transition);
static GstCaps * gst_avf_video_src_get_caps (GstBaseSrc * basesrc,
    GstCaps * filter);
static gboolean gst_avf_video_src_set_caps (GstBaseSrc * basesrc,
    GstCaps * caps);
static gboolean gst_avf_video_src_start (GstBaseSrc * basesrc);
static gboolean gst_avf_video_src_stop (GstBaseSrc * basesrc);
static gboolean gst_avf_video_src_query (GstBaseSrc * basesrc,
    GstQuery * query);
static gboolean gst_avf_video_src_decide_allocation (GstBaseSrc * basesrc,
    GstQuery * query);
static gboolean gst_avf_video_src_unlock (GstBaseSrc * basesrc);
static gboolean gst_avf_video_src_unlock_stop (GstBaseSrc * basesrc);
static GstFlowReturn gst_avf_video_src_create (GstPushSrc * pushsrc,
    GstBuffer ** buf);
static gboolean gst_avf_video_src_negotiate (GstBaseSrc * basesrc);


static void
gst_avf_video_src_class_init (GstAVFVideoSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
  GstPushSrcClass *gstpushsrc_class = GST_PUSH_SRC_CLASS (klass);

  gobject_class->finalize = gst_avf_video_src_finalize;
  gobject_class->get_property = gst_avf_video_src_get_property;
  gobject_class->set_property = gst_avf_video_src_set_property;

  gstelement_class->change_state = gst_avf_video_src_change_state;

  gstbasesrc_class->get_caps = gst_avf_video_src_get_caps;
  gstbasesrc_class->set_caps = gst_avf_video_src_set_caps;
  gstbasesrc_class->start = gst_avf_video_src_start;
  gstbasesrc_class->stop = gst_avf_video_src_stop;
  gstbasesrc_class->query = gst_avf_video_src_query;
  gstbasesrc_class->unlock = gst_avf_video_src_unlock;
  gstbasesrc_class->unlock_stop = gst_avf_video_src_unlock_stop;
  gstbasesrc_class->decide_allocation = gst_avf_video_src_decide_allocation;
  gstbasesrc_class->negotiate = gst_avf_video_src_negotiate;

  gstpushsrc_class->create = gst_avf_video_src_create;

  gst_element_class_set_metadata (gstelement_class,
      "Video Source (AVFoundation)", "Source/Video",
      "Reads frames from an iOS AVFoundation device",
      "Ole André Vadla Ravnås <oleavr@soundrop.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_template));

  g_object_class_install_property (gobject_class, PROP_SESSION,
      g_param_spec_pointer ("session", "Session",
          "Pointer to AVCaptureSession to use instead of internal session (assumes already running)",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_DEVICE_INDEX,
      g_param_spec_int ("device-index", "Device Index",
          "The zero-based device index",
          -1, G_MAXINT, DEFAULT_DEVICE_INDEX,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_DO_STATS,
      g_param_spec_boolean ("do-stats", "Enable statistics",
          "Enable logging of statistics", DEFAULT_DO_STATS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_FPS,
      g_param_spec_int ("fps", "Frames per second",
          "Last measured framerate, if statistics are enabled",
          -1, G_MAXINT, -1, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  /* Default orientation set to the iOS default */
  g_object_class_install_property (gobject_class, PROP_ORIENTATION,
      g_param_spec_enum ("orientation", "Orientation",
          "Image buffer orientation", GST_TYPE_AVFVIDEOSRC_ORIENTATION,
          AVCaptureVideoOrientationLandscapeRight, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
#if !HAVE_IOS
  g_object_class_install_property (gobject_class, PROP_CAPTURE_SCREEN,
      g_param_spec_boolean ("capture-screen", "Enable screen capture",
          "Enable screen capture functionality", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_CAPTURE_SCREEN_CURSOR,
      g_param_spec_boolean ("capture-screen-cursor", "Capture screen cursor",
          "Enable cursor capture while capturing screen", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_CAPTURE_SCREEN_MOUSE_CLICKS,
      g_param_spec_boolean ("capture-screen-mouse-clicks", "Enable mouse clicks capture",
          "Enable mouse clicks capture while capturing screen", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
#endif

  GST_DEBUG_CATEGORY_INIT (gst_avf_video_src_debug, "avfvideosrc",
      0, "iOS AVFoundation video source");
}

#define OBJC_CALLOUT_BEGIN() \
  NSAutoreleasePool *pool; \
  \
  pool = [[NSAutoreleasePool alloc] init]
#define OBJC_CALLOUT_END() \
  [pool release]


static void
gst_avf_video_src_init (GstAVFVideoSrc * src)
{
  OBJC_CALLOUT_BEGIN ();
  src->impl = [[GstAVFVideoSrcImpl alloc] initWithSrc:GST_PUSH_SRC (src)];
  OBJC_CALLOUT_END ();
}

static void
gst_avf_video_src_finalize (GObject * obj)
{
  OBJC_CALLOUT_BEGIN ();
  [GST_AVF_VIDEO_SRC_IMPL (obj) release];
  OBJC_CALLOUT_END ();

  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

static void
gst_avf_video_src_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstAVFVideoSrcImpl *impl = GST_AVF_VIDEO_SRC_IMPL (object);

  switch (prop_id) {
#if !HAVE_IOS
    case PROP_CAPTURE_SCREEN:
      g_value_set_boolean (value, impl.captureScreen);
      break;
    case PROP_CAPTURE_SCREEN_CURSOR:
      g_value_set_boolean (value, impl.captureScreenCursor);
      break;
    case PROP_CAPTURE_SCREEN_MOUSE_CLICKS:
      g_value_set_boolean (value, impl.captureScreenMouseClicks);
      break;
#endif
    case PROP_SESSION:
      g_value_set_pointer (value, impl.externalSession);
      break;
    case PROP_DEVICE_INDEX:
      g_value_set_int (value, impl.deviceIndex);
      break;
    case PROP_DO_STATS:
      g_value_set_boolean (value, impl.doStats);
      break;
    case PROP_FPS:
      GST_OBJECT_LOCK (object);
      g_value_set_int (value, impl.fps);
      GST_OBJECT_UNLOCK (object);
      break;
    case PROP_ORIENTATION:
      GST_OBJECT_LOCK (object);
      g_value_set_enum (value, impl.orientation);
      GST_OBJECT_UNLOCK (object);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_avf_video_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAVFVideoSrcImpl *impl = GST_AVF_VIDEO_SRC_IMPL (object);

  switch (prop_id) {
#if !HAVE_IOS
    case PROP_CAPTURE_SCREEN:
      impl.captureScreen = g_value_get_boolean (value);
      break;
    case PROP_CAPTURE_SCREEN_CURSOR:
      impl.captureScreenCursor = g_value_get_boolean (value);
      break;
    case PROP_CAPTURE_SCREEN_MOUSE_CLICKS:
      impl.captureScreenMouseClicks = g_value_get_boolean (value);
      break;
#endif
    case PROP_ORIENTATION:
      impl.orientation = g_value_get_enum (value);
      [impl updateConnectionOrientation];
      break;
    case PROP_SESSION:
      if (impl.session == nil) {
        impl.externalSession = g_value_get_pointer (value);
      } else {
        GST_WARNING_OBJECT (object, "Cannot change session while running");
      }
      break;
    case PROP_DEVICE_INDEX:
      impl.deviceIndex = g_value_get_int (value);
      break;
    case PROP_DO_STATS:
      impl.doStats = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstStateChangeReturn
gst_avf_video_src_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;

  OBJC_CALLOUT_BEGIN ();
  ret = [GST_AVF_VIDEO_SRC_IMPL (element) changeState: transition];
  OBJC_CALLOUT_END ();

  return ret;
}

static GstCaps *
gst_avf_video_src_get_caps (GstBaseSrc * basesrc, GstCaps * filter)
{
  GstCaps *ret;

  OBJC_CALLOUT_BEGIN ();
  ret = [GST_AVF_VIDEO_SRC_IMPL (basesrc) getCaps];
  OBJC_CALLOUT_END ();

  return ret;
}

static gboolean
gst_avf_video_src_set_caps (GstBaseSrc * basesrc, GstCaps * caps)
{
  gboolean ret;

  OBJC_CALLOUT_BEGIN ();
  ret = [GST_AVF_VIDEO_SRC_IMPL (basesrc) setCaps:caps];
  OBJC_CALLOUT_END ();

  return ret;
}

static gboolean
gst_avf_video_src_start (GstBaseSrc * basesrc)
{
  gboolean ret;

  OBJC_CALLOUT_BEGIN ();
  ret = [GST_AVF_VIDEO_SRC_IMPL (basesrc) start];
  OBJC_CALLOUT_END ();

  return ret;
}

static gboolean
gst_avf_video_src_stop (GstBaseSrc * basesrc)
{
  gboolean ret;

  OBJC_CALLOUT_BEGIN ();
  ret = [GST_AVF_VIDEO_SRC_IMPL (basesrc) stop];
  OBJC_CALLOUT_END ();

  return ret;
}

static gboolean
gst_avf_video_src_query (GstBaseSrc * basesrc, GstQuery * query)
{
  gboolean ret;

  OBJC_CALLOUT_BEGIN ();
  ret = [GST_AVF_VIDEO_SRC_IMPL (basesrc) query:query];
  OBJC_CALLOUT_END ();

  return ret;
}

static gboolean
gst_avf_video_src_decide_allocation (GstBaseSrc * basesrc, GstQuery * query)
{
  gboolean ret;

  OBJC_CALLOUT_BEGIN ();
  ret = [GST_AVF_VIDEO_SRC_IMPL (basesrc) decideAllocation:query];
  OBJC_CALLOUT_END ();

  return ret;
}

static gboolean
gst_avf_video_src_unlock (GstBaseSrc * basesrc)
{
  gboolean ret;

  OBJC_CALLOUT_BEGIN ();
  ret = [GST_AVF_VIDEO_SRC_IMPL (basesrc) unlock];
  OBJC_CALLOUT_END ();

  return ret;
}

static gboolean
gst_avf_video_src_unlock_stop (GstBaseSrc * basesrc)
{
  gboolean ret;

  OBJC_CALLOUT_BEGIN ();
  ret = [GST_AVF_VIDEO_SRC_IMPL (basesrc) unlockStop];
  OBJC_CALLOUT_END ();

  return ret;
}

static GstFlowReturn
gst_avf_video_src_create (GstPushSrc * pushsrc, GstBuffer ** buf)
{
  GstFlowReturn ret;

  OBJC_CALLOUT_BEGIN ();
  ret = [GST_AVF_VIDEO_SRC_IMPL (pushsrc) create: buf];
  OBJC_CALLOUT_END ();

  return ret;
}

static gboolean
gst_avf_video_src_negotiate (GstBaseSrc * basesrc)
{
  /* FIXME: We don't support reconfiguration yet */
  if (gst_pad_has_current_caps (GST_BASE_SRC_PAD (basesrc)))
    return TRUE;

  return GST_BASE_SRC_CLASS (parent_class)->negotiate (basesrc);
}

