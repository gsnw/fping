AM_CFLAGS = -Wall -Wextra -Wno-sign-compare

sbin_PROGRAMS = fping

fping_SOURCES = fping.c flags.c output.c stats.c seqmap.c socket4.c fping.h flags.h options.h output.h stats.h seqmap.h optparse.c optparse.h
fping_DEPENDENCIES = ../config.h

if IPV6
fping_SOURCES += socket6.c
fping_CFLAGS = $(AM_CFLAGS) -DIPV6
endif

if ENABLE_NCURSES
sbin_PROGRAMS += fping-tui
fping_tui_SOURCES = fping-tui.c
fping_tui_CFLAGS  = $(AM_CFLAGS) -DNCURSES_WIDECHAR
fping_tui_LDADD   = $(NCURSES_LIBS)
endif