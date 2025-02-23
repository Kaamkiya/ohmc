.POSIX:

VERSION = 0.0.1

PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man
DOCPREFIX = $(PREFIX)/share/doc

SRC = ohmc.c
OBJ = $(SRC:.c=.o)

OHMC_CFLAGS = $(CFLAGS) -Wall -Werror -Wpedantic -std=c99
OHMC_LDFLAGS = $(LDFLAGS)

all: ohmc

.c.o:
	$(CC) -c $< $(OHMC_CFLAGS) $(OHMC_CPPFLAGS)

ohmc: $(OBJ) $(LIBS)
	$(CC) -o $@ $(OBJ) $(LIBS) $(OHMC_LDFLAGS)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	mkdir -p $(DESTDIR)$(DOCPREFIX)/ohmc
	install -m 644 README LICENSE $(DESTDIR)$(DOCPREFIX)/ohmc
	install -m 775 ohmc $(DESTDIR)$(PREFIX)/bin

uninstall: all
	rm -f $(DESTDIR)$(PREFIX)/bin/ohmc
	rm -rf $(DESTDIR)$(DOCPREFIX)/ohmc

dist: clean
	mkdir -p ohmc-$(VERSION)
	cp -R Makefile README LICENSE ohmc.c ohmc-$(VERSION)
	tar -cf - ohmc-$(VERSION) | gzip -c > ohmc-$(VERSION).tar.gz
	rm -rf ohmc-$(VERSION)

clean:
	rm -f ohmc *.o
