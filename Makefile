CFLAGS ?= -O2 -Wall -Wextra
# setup.sh resolves the install paths: make would expand "$" in them and paste
# the result into shell code.

statusline: statusline.c
	$(CC) $(CFLAGS) -o $@ $<

copy: statusline
	@sh ./setup.sh copy

# Edits settings.json only when it does not run this binary yet.
install: copy
	@sh ./setup.sh install

uninstall:
	@sh ./setup.sh remove

clean:
	rm -f statusline

.PHONY: copy install uninstall clean
