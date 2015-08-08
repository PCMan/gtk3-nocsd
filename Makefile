PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CFLAGS += $(shell ${PKG_CONFIG} --cflags gtk+-3.0) -pthread -Wall -fPIC
LDLIBS = -ldl

prefix ?= /usr/local
libdir ?= $(prefix)/lib

all: libgtk3-nocsd.so.0 wrapper.sh

clean:
	rm -f libgtk3-nocsd.so.0 *.o wrapper.sh *~

libgtk3-nocsd.so.0: gtk3-nocsd.o
	$(CC) -shared $(CFLAGS) $(LDFLAGS) -Wl,-soname,libgtk3-nocsd.so.0 -o $@ $^ $(LDLIBS)

wrapper.sh: wrapper.sh.in
	sed 's|@@libdir@@|$(libdir)|g' < $< > $@
	chmod +x wrapper.sh

install:
	install -D -m 0644 libgtk3-nocsd.so.0 $(DESTDIR)$(libdir)/libgtk3-nocsd.so.0
