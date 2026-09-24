all := libaxil-tty
SONAME-libaxil-tty := axil-tty

LDLIBS-libaxil-tty := -laxil -lcorm -lxylem
LDFLAGS-libaxil-tty-Darwin := -undefined dynamic_lookup

share != find ./htdocs -type f
share-dir := axil

-include ./../mk/include.mk

CFLAGS += -g \
	-DAXIL_PREFIX='"$(PREFIX)"' \
	-DAXIL_HTDOCS='"$(PREFIX)/share/axil/htdocs"'