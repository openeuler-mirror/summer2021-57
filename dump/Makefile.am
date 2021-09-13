# SPDX-License-Identifier: GPL-2.0+
# Makefile.am

AUTOMAKE_OPTIONS = foreign
bin_PROGRAMS     = dump.erofs
AM_CPPFLAGS = ${libuuid_CFLAGS}
dump_erofs_SOURCES = main.c
dump_erofs_CFLAGS = -Wall -Werror -I$(top_srcdir)/include
dump_erofs_LDADD = ${libuuid_LIBS} $(top_builddir)/lib/liberofs.la ${liblz4_LIBS}

