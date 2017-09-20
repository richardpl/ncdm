PREFIX=/usr/local

CFLAGS  = -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -O3 -std=c99 -Wall -Wextra -g `curl-config --cflags`
LIBS    = `curl-config --libs` -lncursesw -levent -lpthread
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
