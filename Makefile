PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CFLAGS += $(shell ${PKG_CONFIG} --cflags gtk+-3.0) $(shell ${PKG_CONFIG} --cflags gobject-introspection-1.0) -pthread -Wall -fPIC
LDLIBS = -ldl

prefix ?= /usr/local
libdir ?= $(prefix)/lib
bindir ?= $(prefix)/bin
mandir ?= $(prefix)/share/man

all: libgtk3-nocsd.so.0 gtk3-nocsd

clean:
	rm -f libgtk3-nocsd.so.0 *.o gtk3-nocsd *~

libgtk3-nocsd.so.0: gtk3-nocsd.o
	$(CC) -shared $(CFLAGS) $(LDFLAGS) -Wl,-soname,libgtk3-nocsd.so.0 -o $@ $^ $(LDLIBS)

gtk3-nocsd: gtk3-nocsd.in
	sed 's|@@libdir@@|$(libdir)|g' < $< > $@
	chmod +x $@

install:
	install -D -m 0644 libgtk3-nocsd.so.0 $(DESTDIR)$(libdir)/libgtk3-nocsd.so.0
	install -D -m 0755 gtk3-nocsd $(DESTDIR)$(bindir)/gtk3-nocsd
	install -D -m 0644 gtk3-nocsd.1 $(DESTDIR)$(mandir)/man1/gtk3-nocsd.1
