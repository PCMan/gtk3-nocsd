#!/bin/sh

: ${PKG_CONFIG:=pkg-config}
: ${CC:=cc}
: ${CFLAGS:=-O2 -g}

set -x

${CC} `${PKG_CONFIG} --cflags gtk+-3.0` ${CPPFLAGS} ${CFLAGS} -Wall -fPIC -shared ${LDFLAGS} -ldl -o gtk3-nocsd.so gtk3-nocsd.c
