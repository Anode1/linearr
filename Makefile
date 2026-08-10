# Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE.
#
# ais-style build. Honors the standard variables (CC CFLAGS CPPFLAGS LDFLAGS
# LDLIBS); project-required flags are APPENDED, never override yours. The one
# library the project adds for you is -lm, which the fit and the rounding need.
# Drop in a .c (here or one directory down) and it compiles, no editing
# this file.
#   make | release | debug | pedantic | check | ut | cliut | ut-asan | ut-ubsan
#   | hooks | clean
SHELL = /bin/sh

BIN     = linearr
TESTBIN = linearr_ut

CC       ?= cc
CFLAGS   ?= -O2
CPPFLAGS ?=
LDFLAGS  ?=
LDLIBS   ?=

# project-required flags, applied alongside (not over) the user's
STD  = -std=c99
WARN = -W -Wall
PROJ = $(STD) $(WARN)
LIBM = -lm

# every *.c at top level or one dir down; add a file, no edit needed
SOURCES.c := $(wildcard *.c) $(wildcard */*.c)
OBJS       = $(SOURCES.c:.c=.o)

debug    : CFLAGS = -g -O0
debug    : WARN  += -Wundef
pedantic : STD    = -std=c99 -pedantic
pedantic : WARN  += -Wshadow -Wundef -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations
release  : CFLAGS = -O2

.SUFFIXES:
.SUFFIXES: .d .o .h .c

# .build-flags records the flags the objects were compiled with, and every .o
# depends on it. Without this, `make` then
#     make CPPFLAGS='-DREGRESS_MAX_VARS=32 -DLOS_MAX_VARS=32'
# (the exact command the README gives for setting the term ceiling) printed
# "Nothing to be done for 'all'", exited 0, and left you the 256-term binary
# while you believed you had a 32-term one. Nothing in a .o file records the
# macros it was built with, so make had no way to know. The stamp gives it one.
#
# It is done with $(shell) at parse time and NOT with a rule depending on a
# .PHONY target: writing it that way puts a real target above `all:` and hands
# the default goal to it, which is precisely the defect this project already
# fixed once.
#
# It also DELETES the objects rather than making them depend on the stamp file.
# The dependency version looked cleaner and was quietly broken: make caches the
# directory listing at startup, so a file $(shell) creates during parsing is not
# visible to it, the prerequisite `.build-flags` appears not to exist, the
# pattern rule below is rejected as inapplicable, and make falls back to its
# BUILT-IN %.o rule. That rule does not carry $(PROJ), so a fresh tree built
# with any CPPFLAGS compiled without -std=c99 -W -Wall and without -MMD: no
# warnings, no header dependencies. A deliberately uninitialised variable
# produced zero diagnostics, while the first rule in the README's Style section
# is that a warning is a defect. The build was not looking.
#
# Deleting is also immune to timestamp granularity, which the mtime comparison
# was not.
FLAGS_NOW := $(CC) $(PROJ) $(CPPFLAGS) $(CFLAGS)
FLAGS_WAS := $(shell cat .build-flags 2>/dev/null)
ifneq ($(FLAGS_NOW),$(FLAGS_WAS))
$(shell rm -f $(OBJS) $(OBJS:.o=.d) $(BIN) $(TESTBIN) $(TESTBIN)_asan $(TESTBIN)_ubsan; \
        printf '%s\n' "$(FLAGS_NOW)" > .build-flags)
endif

%.o: %.c
	$(CC) $(PROJ) $(CPPFLAGS) $(CFLAGS) -MMD -c $< -o $@

.PHONY: all release debug pedantic check ut cliut readme ut-asan ut-ubsan hooks \
        install uninstall clean modeclean

PREFIX ?= /usr/local
DESTDIR ?=

# all comes FIRST on purpose: make's default goal is the first non-special
# target in the file, and .PHONY / .SUFFIXES / pattern rules do not count. With
# modeclean written above this line, a bare `make` (the command the README
# leads with) quietly ran `rm -f` on the objects and the binary and reported
# success, building nothing. It looked like an up-to-date no-op.
all: $(BIN)

# A mode target changes CFLAGS, and a target-specific variable does NOT invalidate
# an existing .o. So `make` then `make debug` reported "Nothing to be done" and
# left you the -O2 objects while you believed you had a debug build. Each mode
# drops the objects first; `all` does not, so the ordinary edit-build loop is
# still incremental.
modeclean:
	@rm -f $(OBJS) $(OBJS:.o=.d) $(BIN)

release debug pedantic: modeclean
	@$(MAKE) --no-print-directory $(BIN) STD='$(STD)' WARN='$(WARN)' CFLAGS='$(CFLAGS)'

$(BIN): $(OBJS)
	$(CC) $(PROJ) $(CFLAGS) $(LDFLAGS) -o $(BIN) $(OBJS) $(LDLIBS) $(LIBM)

# ut: all sources with -DUNIT_TEST: main.c's main() compiles out, tests.c's in.
# Run from the project root: the tests read conf/ and example/ by relative path.
$(TESTBIN): $(SOURCES.c) .build-flags
	$(CC) $(PROJ) -g -DUNIT_TEST $(CPPFLAGS) $(SOURCES.c) -o $(TESTBIN) $(LDLIBS) $(LIBM)
ut: $(TESTBIN)
	./$(TESTBIN)

# cliut: the built binary driven through the shell, which is the only place some
# behaviour exists at all. `make ut` never has a terminal on stdin, so it could
# not see that a bare `./linearr` sat there looking hung instead of printing its
# usage. Needs the binary, not the test build.
cliut: $(BIN)
	@sh tests/cli.sh

# readme: run every command the README prints and diff its output. Three
# reviews found stale transcripts here, and the third found fixes reported as
# done that had never landed. Prose can be proof-read; a transcript has to be
# executed.
readme: $(BIN)
	@python3 scripts/readme-check.py

# check: all three gates. Run it before a commit.
check: ut cliut readme

# ut-asan / ut-ubsan: the same tests under AddressSanitizer and under
# UndefinedBehaviorSanitizer. A leak, an overflow, or UB aborts with a file:line
# report instead of passing silently under -O2. Run both before tagging; the
# pre-push hook (make hooks) and .github/workflows/sanitizers.yml do it for you.
# A fresh build each time, not the plain objects.
ut-asan: $(SOURCES.c) .build-flags
	$(CC) $(PROJ) -g -DUNIT_TEST -fsanitize=address -fno-omit-frame-pointer \
		$(CPPFLAGS) $(SOURCES.c) -o $(TESTBIN)_asan $(LDLIBS) $(LIBM)
	./$(TESTBIN)_asan

ut-ubsan: $(SOURCES.c) .build-flags
	$(CC) $(PROJ) -g -DUNIT_TEST -fsanitize=undefined -fno-sanitize-recover=undefined \
		-fno-omit-frame-pointer $(CPPFLAGS) $(SOURCES.c) -o $(TESTBIN)_ubsan $(LDLIBS) $(LIBM)
	./$(TESTBIN)_ubsan

# hooks: point git at scripts/hooks, so pre-push runs the sanitizers locally
# before anything reaches the remote. Bypass once with `git push --no-verify`.
hooks:
	git config core.hooksPath scripts/hooks
	@echo "hooks enabled: scripts/hooks/pre-push runs ut-asan + ut-ubsan"

# install: the binary on PATH, its data in share. resolve.c looks in the current
# directory, then beside the binary, then <bindir>/../share/linearr, so this
# layout works and a symlink into a bin directory works too (the program
# resolves the link before looking beside itself).
#   make install                      -> /usr/local
#   make install PREFIX=$$HOME/.local  -> ~/.local
#   make install DESTDIR=/tmp/stage   -> staged, for a package
install: $(BIN)
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/$(BIN)/conf
	cp $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	cp conf/*.csv $(DESTDIR)$(PREFIX)/share/$(BIN)/conf/
	cp system.properties $(DESTDIR)$(PREFIX)/share/$(BIN)/ 2>/dev/null || true
	@echo "installed $(BIN) to $(DESTDIR)$(PREFIX)/bin"
	@echo "example tables in $(DESTDIR)$(PREFIX)/share/$(BIN)/conf (use -c for your own)"

uninstall:
	-rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	-rm -rf $(DESTDIR)$(PREFIX)/share/$(BIN)

clean:
	-rm -f $(BIN) $(TESTBIN) $(TESTBIN)_asan $(TESTBIN)_ubsan \
	       $(OBJS) $(OBJS:.o=.d) $(OBJS:.o=.su) .build-flags

-include $(OBJS:.o=.d)
