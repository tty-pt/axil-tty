all := libaxil-tty
SONAME-libaxil-tty := axil-tty

# -I../axil/include, ahead of /usr/include: this module is loaded from
# ../axil/lib (see the site's LD_LIBRARY_PATH), so it must be built against the
# headers of the library it actually binds to. Without it the build resolves
# <ttypt/axil.h> from a system copy that `sudo make install` refreshes by hand,
# which silently breaks as soon as a new axil entry point is added here --
# "implicit declaration of function axil_generation". Same pattern as the
# -I../axil-tty/include in axil-nd/Makefile.

LDLIBS-libaxil-tty := -laxil -lcorm -lxylem
LDFLAGS-libaxil-tty-Darwin := -undefined dynamic_lookup

share != find ./htdocs -type f
share-dir := axil

-include ./../mk/include.mk

CFLAGS += -g \
	-I../axil/include \
	-DAXIL_PREFIX='"$(PREFIX)"' \
	-DAXIL_HTDOCS='"$(PREFIX)/share/axil/htdocs"'