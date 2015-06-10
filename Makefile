PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CFLAGS += $(shell ${PKG_CONFIG} --cflags gtk+-3.0) -pthread -Wall -fPIC
LDLIBS = -ldl

libdir ?= /usr/lib

all: libgtk3-nocsd.so

clean:
	rm -f libgtk3-nocsd.so *.o *~

libgtk3-nocsd.so: gtk3-nocsd.o
	$(CC) -shared $(CFLAGS) $(LDFLAGS) $(LDLIBS) -o $@ $^

install:
	install -D -m 0644 libgtk3-nocsd.so $(DESTDIR)$(libdir)/libgtk3-nocsd.so
