#!/bin/sh
gcc `pkg-config --cflags gtk+-3.0` -Wall -fPIC -shared -ldl -o gtk3-nocsd.so gtk3-nocsd.c
