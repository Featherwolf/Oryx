# oryx-perf — no-root on-device performance probe. Builds natively (Termux/any Linux).
CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: oryx-perf

oryx-perf: oryx-perf.c
	$(CC) $(CFLAGS) -o $@ oryx-perf.c

# quick host smoke: 3 s at 4 Hz (CPU util works anywhere; GPU/thermal are phone-only)
smoke: oryx-perf
	@./oryx-perf --secs 3 --hz 4 --csv /tmp/oryx-perf-smoke.csv

clean:
	rm -f oryx-perf

.PHONY: all smoke clean
