# xcat  — X11 filters for a Unix pipeline
#
# Needs libX11 and libXft. On Debian/Ubuntu: apt install libx11-dev libxft-dev
# On Fedora: dnf install libX11-devel libXft-devel
# On macOS (XQuartz): xcode + brew or the XQuartz SDK under /opt/X11

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2
PREFIX  ?= /usr/local
PROG = xcat

PKG_CFLAGS := $(shell pkg-config --cflags x11 xft 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs x11 xft 2>/dev/null)

ifeq ($(PKG_LIBS),)
  X11_CFLAGS ?= -I/usr/include -I/usr/include/freetype2 -I/opt/X11/include -I/opt/X11/include/freetype2
  X11_LIBS   ?= -lXft -lXrender -lX11 -lfontconfig -lfreetype
else
  X11_CFLAGS ?= $(PKG_CFLAGS)
  X11_LIBS   ?= $(PKG_LIBS)
endif

.PHONY: all clean install

all: xcat 

xcat: xcat.c
	$(CC) $(CFLAGS) $(X11_CFLAGS) -o $@ xcat.c $(X11_LIBS) -lm

install: xcat 
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 xcat $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -f xcat 


backup:
	@read -p "Comment for backup: " comment; \
	stamp=$$(date +%Y%m%d-%H%M%S); \
	mkdir -p old; \
	echo "$$comment" > $(PROG)-$${stamp}.txt; \
	tar cf old/$(PROG)-$${stamp}.tar $(PROG)-$${stamp}.txt \
		$(SRCS) Makefile  ;  \
	echo "backed up to old/$(PROG)-$${stamp}.tar ($$comment)"; \
	tar -tvf old/$(PROG)-$${stamp}.tar ; \
	mv $(PROG)-$${stamp}.txt old ; \
	ls -l old/$(PROG)-$${stamp}.tar

.PHONY: all clean
