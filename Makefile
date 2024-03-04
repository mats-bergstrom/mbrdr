######################### -*- Mode: Makefile-Gmake -*- ########################
## Copyright (C) 2024, Mats Bergstrom
## $Id$
## 
## File name       : Makefile
## Description     : for mbrdr
## 
## Author          : Mats Bergstrom
## Created On      : Thu Feb 29 19:47:26 2024
## 
## Last Modified By: Mats Bergstrom
## Last Modified On: Mon Mar  4 12:33:04 2024
## Update Count    : 10
###############################################################################


CC = gcc
CFLAGS = -Wall -pedantic-errors  -g
CPPFLAGS = -Ilibmodbus/src -Icfgf
LDLIBS = -lmosquitto -lmodbus -lcfgf
LDFLAGS = -L.  -Lcfgf

IBIN	= /usr/local/bin
ETCDIR	= /usr/local/etc
SYSTEMD_DIR = /lib/systemd/system

BINARIES = mbrdr
CFGS = mbrdr.cfg
SYSTEMD_FILES = mbrdr.service

all: mbrdr

mbrdr: mbrdr.o

mbrdr.o: mbrdr.c

.PHONY: clean uninstall install


clean:
	cd libmodbus && make clean
	cd cfgf && make clean
	rm -f libmodbus.a *.o mbrdr *~ *.log .*~

uninstall:
	cd $(SYSTEMD_DIR); rm $(SYSTEMD_FILES)
	cd $(IBIN); rm $(BINARIES)

install:
	if [ ! -d $(IBIN) ] ; then mkdir $(IBIN); fi
	if [ ! -d $(ETCDIR) ] ; then mkdir $(ETCDIR); fi
	cp $(BINARIES) $(IBIN)
	cp $(CFGS) $(ETCDIR)
	cp $(SYSTEMD_FILES) $(SYSTEMD_DIR)

