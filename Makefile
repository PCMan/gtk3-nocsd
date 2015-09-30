PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CFLAGS += $(shell ${PKG_CONFIG} --cflags gtk+-3.0) $(shell ${PKG_CONFIG} --cflags gobject-introspection-1.0) -pthread -Wall -fPIC
LDLIBS = -ldl

libdir ?= /usr/lib

all: libgtk3-nocsd.so.0

clean:
	rm -f libgtk3-nocsd.so.0 *.o *~

libgtk3-nocsd.so.0: gtk3-nocsd.o
	$(CC) -shared $(CFLAGS) $(LDFLAGS) -Wl,-soname,libgtk3-nocsd.so.0 -o $@ $^ $(LDLIBS)

install:
	install -D -m 0644 libgtk3-nocsd.so.0 $(DESTDIR)$(libdir)/libgtk3-nocsd.so.0
