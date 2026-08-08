# Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later.
#
# ais-style build. Honors the standard variables (CC CFLAGS CPPFLAGS LDFLAGS
# LDLIBS); project-required flags are APPENDED, never override yours. The one
# library the project adds for you is -lm, which the fit and the rounding need.
# Drop in a .c -- here or one directory down -- and it compiles, no editing
# this file.
#   make | release | debug | pedantic | ut | ut-asan | ut-ubsan | hooks | clean
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
%.o: %.c
	$(CC) $(PROJ) $(CPPFLAGS) $(CFLAGS) -MMD -c $< -o $@

.PHONY: all release debug pedantic ut ut-asan ut-ubsan hooks clean modeclean

# all comes FIRST on purpose: make's default goal is the first non-special
# target in the file, and .PHONY / .SUFFIXES / pattern rules do not count. With
# modeclean written above this line, a bare `make` -- the command the README
# leads with -- quietly ran `rm -f` on the objects and the binary and reported
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

# ut: all sources with -DUNIT_TEST -- main.c's main() compiles out, tests.c's in.
# Run from the project root: the tests read conf/ and example/ by relative path.
$(TESTBIN): $(SOURCES.c)
	$(CC) $(PROJ) -g -DUNIT_TEST $(CPPFLAGS) $(SOURCES.c) -o $(TESTBIN) $(LDLIBS) $(LIBM)
ut: $(TESTBIN)
	./$(TESTBIN)

# ut-asan / ut-ubsan: the same tests under AddressSanitizer and under
# UndefinedBehaviorSanitizer. A leak, an overflow, or UB aborts with a file:line
# report instead of passing silently under -O2. Run both before tagging; the
# pre-push hook (make hooks) and .github/workflows/sanitizers.yml do it for you.
# A fresh build each time, not the plain objects.
ut-asan: $(SOURCES.c)
	$(CC) $(PROJ) -g -DUNIT_TEST -fsanitize=address -fno-omit-frame-pointer \
		$(CPPFLAGS) $(SOURCES.c) -o $(TESTBIN) $(LDLIBS) $(LIBM)
	./$(TESTBIN)

ut-ubsan: $(SOURCES.c)
	$(CC) $(PROJ) -g -DUNIT_TEST -fsanitize=undefined -fno-sanitize-recover=undefined \
		-fno-omit-frame-pointer $(CPPFLAGS) $(SOURCES.c) -o $(TESTBIN) $(LDLIBS) $(LIBM)
	./$(TESTBIN)

# hooks: point git at scripts/hooks, so pre-push runs the sanitizers locally
# before anything reaches the remote. Bypass once with `git push --no-verify`.
hooks:
	git config core.hooksPath scripts/hooks
	@echo "hooks enabled: scripts/hooks/pre-push runs ut-asan + ut-ubsan"

clean:
	-rm -f $(BIN) $(TESTBIN) $(OBJS) $(OBJS:.o=.d)

-include $(OBJS:.o=.d)
