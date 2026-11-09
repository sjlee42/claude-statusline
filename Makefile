CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
TARGET := $(HOME)/.claude/statusline

statusline: statusline.c
	$(CC) $(CFLAGS) -o $@ $<

install: statusline
	cp statusline $(TARGET)

clean:
	rm -f statusline

.PHONY: install clean
