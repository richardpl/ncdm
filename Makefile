PREFIX=/usr/local

CFLAGS = -D_FILE_OFFSET_BITS=64 -O3 -std=c99 -Wall -Wextra -g

OBJECTS = main.o

ncdm: $(OBJECTS)
	$(CC) -o $@ $^ -lcurl -lncursesw $(LIBS)

distclean: clean

clean:
	@rm -f $(OBJECTS) ncdm

install: ncdm
	@install -v ncdm $(PREFIX)/bin/

uninstall:
	@rm -fv $(PREFIX)/bin/ncdm

all: ncdm
