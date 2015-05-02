lib_LTLIBRARIES = libgstapplemedia-@GST_API_VERSION@.la

noinst_HEADERS =

libgstapplemedia_@GST_API_VERSION@_la_SOURCES = \
  corevideomemory.c \
  corevideobuffer.c \
  coremediabuffer.c

libgstapplemedia_@GST_API_VERSION@_la_CFLAGS = \
  -I$(top_srcdir)/gst-libs \
  -I$(top_builddir)/gst-libs \
  $(GST_CFLAGS) \
  $(GST_BASE_CFLAGS) \
  $(GST_PLUGINS_BASE_CFLAGS)

libgstapplemedia_@GST_API_VERSION@_la_OBJCFLAGS =			\
  -I$(top_srcdir)/gst-libs \
  -I$(top_builddir)/gst-libs \
  $(GST_OBJCFLAGS) \
  $(GST_BASE_CFLAGS) \
  $(GST_PLUGINS_BASE_CFLAGS)

if HAVE_IOS
libgstapplemedia_@GST_API_VERSION@_la_OBJCFLAGS += -fobjc-abi-version=2 -fobjc-legacy-dispatch
endif

libgstapplemedia_@GST_API_VERSION@_la_LIBADD = \
  $(GMODULE_NO_EXPORT_LIBS) \
  $(GST_PLUGINS_BASE_LIBS) \
  -lgstvideo-$(GST_API_VERSION) \
  $(GST_BASE_LIBS) \
  $(GST_LIBS)

libgstapplemedia_@GST_API_VERSION@_la_LDFLAGS = \
  $(GST_LIB_LDFLAGS) \
  $(GST_ALL_LDFLAGS) \
  $(GST_LT_LDFLAGS) \
  -framework CoreFoundation \
  -framework CoreMedia \
  -framework CoreVideo

libgstapplemedia_@GST_API_VERSION@includedir = $(includedir)/gstreamer-@GST_API_VERSION@/gst/applemedia
libgstapplemedia_@GST_API_VERSION@include_HEADERS = \
  corevideomemory.h \
  corevideobuffer.h \
  coremediabuffer.h

