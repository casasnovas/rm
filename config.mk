SHELL := /bin/sh
SRCDIR := $(shell pwd)
DESTDIR := /usr/local/bin
MODDIR := /lib/modules/$(shell uname -r)

PROJECT_NAME = rm
PROJECT_VERSION = 0.1
BINARY_NAME = rm

ARCH := sparc
CROSS_COMPILE := sparc-linux-
