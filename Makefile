PREFIX=/usr/local

CFLAGS  = -D_XOPEN_SOURCE=500 -D_POSIX_C_SOURCE=199309L -D_FILE_OFFSET_BITS=64 -O3 -std=c99 -Wall -Wextra -g `curl-config --cflags`
LIBS    = `curl-config --libs` -lncursesw
SOURCES = main.c
OBJECTS = main.o

ncdm: $(OBJECTS)
	$(CC) -o $@ $(CFLAGS) $(SOURCES) $(LIBS)

distclean: clean

clean:
	@rm -f $(OBJECTS) ncdm

install: ncdm
	@install -v ncdm $(PREFIX)/bin/

uninstall:
	@rm -fv $(PREFIX)/bin/ncdm

all: ncdm
