# Note: plugindir is set in configure

##############################################################################
# TODO: change libgstplugin.la to something else, e.g. libmysomething.la     #
##############################################################################
lib_LTLIBRARIES = libgstframebuffersink.la
plugin_LTLIBRARIES = libgstfbdev2sink.la libgstsunxifbsink.la libgstdrmsink.la

##############################################################################
# TODO: for the next set of variables, name the prefix if you named the .la, #
#  e.g. libmysomething.la => libmysomething_la_SOURCES                       #
#                            libmysomething_la_CFLAGS                        #
#                            libmysomething_la_LIBADD                        #
#                            libmysomething_la_LDFLAGS                       #
##############################################################################

# sources used to compile this library
libgstframebuffersink_la_SOURCES = gstframebuffersink.c gstframebuffersink.h \
    gstfbdevframebuffersink.c gstfbdevframebuffersink.h

# compiler and linker flags used to compile this library, set in configure.ac
libgstframebuffersink_la_CFLAGS = $(GST_CFLAGS)
libgstframebuffersink_la_LIBADD = $(GST_LIBS) 
libgstframebuffersink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstframebuffersink_la_LIBTOOLFLAGS = --tag=disable-static

# sources used to compile this plug-in
libgstfbdev2sink_la_SOURCES = gstfbdev2sink.c gstfbdev2sink.h

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstfbdev2sink_la_CFLAGS = $(GST_CFLAGS)
libgstfbdev2sink_la_LIBADD = $(GST_LIBS) -lgstframebuffersink
libgstfbdev2sink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstfbdev2sink_la_LIBTOOLFLAGS = --tag=disable-static

# sources used to compile this plugin
libgstsunxifbsink_la_SOURCES = gstsunxifbsink.c gstsunxifbsink.h

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstsunxifbsink_la_CFLAGS = $(GST_CFLAGS)
libgstsunxifbsink_la_LIBADD = $(GST_LIBS) -lgstframebuffersink
libgstsunxifbsink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstsunxifbsink_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS = gstframebuffersink.h gstfbdevframebuffersink.h gstfbdev2sink.h \
    gstsunxifbsink.h gstdrmsink.h

# sources used to compile this plugin
libgstdrmsink_la_SOURCES = gstdrmsink.c gstdrmsink.h

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstdrmsink_la_CFLAGS = $(GST_CFLAGS) `pkg-config --cflags libdrm --cflags libkms`
libgstdrmsink_la_LIBADD = $(GST_LIBS) -lgstframebuffersink -ldrm -lkms
libgstdrmsink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstdrmsink_la_LIBTOOLFLAGS = --tag=disable-static
