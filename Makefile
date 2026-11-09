CFLAGS ?= -O2 -Wall -Wextra
# Claude Code's config dir (honors CLAUDE_CONFIG_DIR, like Claude Code itself)
CLAUDE_DIR := $(or $(CLAUDE_CONFIG_DIR),$(HOME)/.claude)
TARGET := $(CLAUDE_DIR)/statusline

statusline: statusline.c
	$(CC) $(CFLAGS) -o $@ $<

# Copy to a new file, then rename: macOS kills a signed binary overwritten in place.
copy: statusline
	@mkdir -p "$(CLAUDE_DIR)"
	cp statusline "$(TARGET).tmp" && mv -f "$(TARGET).tmp" "$(TARGET)"

# Asks for the palette only when settings.json does not run this binary yet.
install: copy
	@sh ./setup.sh --if-new

# Pick the palette again.
setup: copy
	@sh ./setup.sh

uninstall:
	@sh ./setup.sh --remove
	rm -f "$(TARGET)"

clean:
	rm -f statusline

.PHONY: copy install setup uninstall clean
