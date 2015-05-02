
if USE_OPENGL
GL_DIR = gl
endif
if USE_GLES2
GL_DIR = gl
endif

if USE_WAYLAND
WAYLAND_DIR=wayland
endif

if USE_APPLE_MEDIA
APPLE_MEDIA_DIR=applemedia
else
APPLE_MEDIA_DIR=
endif

SUBDIRS = uridownloader adaptivedemux interfaces basecamerabinsrc codecparsers \
	 insertbin mpegts base video $(GL_DIR) $(WAYLAND_DIR) $(APPLE_MEDIA_DIR)

noinst_HEADERS = gst-i18n-plugin.h gettext.h glib-compat-private.h
DIST_SUBDIRS = uridownloader adaptivedemux interfaces gl basecamerabinsrc \
	codecparsers insertbin mpegts wayland base video applemedia

#dependencies
video: base

adaptivedemux: uridownloader

INDEPENDENT_SUBDIRS = \
	interfaces basecamerabinsrc codecparsers insertbin uridownloader \
	mpegts base $(GL_DIR) $(WAYLAND_DIR) $(APPLE_MEDIA_DIR)

.PHONY: independent-subdirs $(INDEPENDENT_SUBDIRS)

independent-subdirs: $(INDEPENDENT_SUBDIRS)

$(INDEPENDENT_SUBDIRS):
	$(MAKE) -C $@

all-recursive: independent-subdirs
