CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -O2 -D_XOPEN_SOURCE=700 \
          $(shell pkg-config --cflags x11 xft)
LDLIBS  = $(shell pkg-config --libs x11 xft)   

SRCS = main.c buffer.c utf8.c display.c menu.c find.c \
       filepicker.c clipboard.c fileio.c input.c
OBJS = $(SRCS:.c=.o)
HDRS = xcat.h buffer.h utf8.h display.h menu.h find.h \
       filepicker.h clipboard.h fileio.h input.h

all: xcat

xcat: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) xcat

.PHONY: all clean   
