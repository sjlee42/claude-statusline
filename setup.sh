#!/bin/sh
# Pick the status line palette and write the statusLine command into Claude
# Code's settings.json (a .bak copy is kept). Called by make:
#   make install    -> setup.sh --if-new   (asks only if settings.json does not run it yet)
#   make setup      -> setup.sh            (asks again)
#   make uninstall  -> setup.sh --remove   (drops the statusLine entry if it is ours)
set -e

DIR="${CLAUDE_CONFIG_DIR:-$HOME/.claude}"
BIN="$DIR/statusline"
SETTINGS="$DIR/settings.json"
if [ "$DIR" = "$HOME/.claude" ]; then CMD_BIN="~/.claude/statusline"; else CMD_BIN="\"$BIN\""; fi
SAMPLE='{"model":{"display_name":"Opus 5.5"},"context_window":{"used_percentage":42},"effort":{"level":"high"},"rate_limits":{"five_hour":{"used_percentage":65,"resets_at":1762700000},"seven_day":{"used_percentage":88,"resets_at":1762900000}}}'
MODE="${1:-ask}"

manual() {
    echo "Add this to $SETTINGS yourself:"
    echo "  \"statusLine\": { \"type\": \"command\", \"command\": \"$1\" }"
}

# settings.json helper: check | write CMD | remove
settings() {
    python3 -I - "$SETTINGS" "$BIN" "$@" <<'EOF'
import json, os, shlex, shutil, sys
path, binary, action = sys.argv[1], sys.argv[2], sys.argv[3]
data = {}
if os.path.exists(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    if text.strip():
        try:
            data = json.loads(text)
        except ValueError as e:
            sys.exit(f"{path} is not valid JSON ({e}); not edited.")
if not isinstance(data, dict):
    sys.exit(f"{path} is not a JSON object; not edited.")

def ours():  # does statusLine.command run this binary?
    sl = data.get("statusLine")
    cmd = sl.get("command") if isinstance(sl, dict) else None
    try:
        exe = shlex.split(cmd)[0] if isinstance(cmd, str) else ""
    except ValueError:
        return False
    exe = os.path.expanduser(os.path.expandvars(exe))
    return bool(exe) and os.path.realpath(exe) == os.path.realpath(binary)

if action == "check":
    sys.exit(0 if ours() else 1)
if action == "remove":
    if not ours():
        sys.exit(1)
    del data["statusLine"]
else:
    data["statusLine"] = {"type": "command", "command": sys.argv[4]}

os.makedirs(os.path.dirname(path), exist_ok=True)
if os.path.exists(path):
    shutil.copy2(path, path + ".bak")
tmp = path + ".tmp"
with open(tmp, "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2, ensure_ascii=False)
    f.write("\n")
if os.path.exists(path):
    shutil.copymode(path, tmp)
os.replace(tmp, path)
EOF
}

has_python() { command -v python3 >/dev/null 2>&1; }

if [ "$MODE" = "--remove" ]; then
    if has_python && settings remove; then
        echo "Removed the statusLine entry from $SETTINGS (backup: $SETTINGS.bak)."
    else
        echo "$SETTINGS was not changed. Remove its statusLine entry yourself if it still runs $BIN."
    fi
    exit 0
fi

# Questions need a terminal (the PostToolUse hook and CI run make install without one).
if [ ! -t 0 ]; then
    [ "$MODE" = "--if-new" ] || echo "setup.sh: not a terminal, skipping palette setup (run 'make setup' later)." >&2
    exit 0
fi
if [ "$MODE" = "--if-new" ] && has_python && settings check; then
    exit 0 # already set up; make install only replaced the binary
fi
if [ ! -x "$BIN" ]; then
    echo "setup.sh: $BIN not found; run 'make setup' instead." >&2
    exit 1
fi

ask() { # prompt default -> REPLY
    printf '%s ' "$1"
    read -r REPLY || REPLY=""
    [ -n "$REPLY" ] || REPLY="$2"
}

echo
echo "claude-statusline setup / 설정"
echo
ask "1) Terminal background / 터미널 배경 — [1] dark 어두움  [2] light 밝음 (default 1):" 1
case "$REPLY" in 2|l|light) BG=--light ;; *) BG=--dark ;; esac

ask "2) Color-blind friendly palette / 색맹 친화 팔레트? [y/N]:" n
case "$REPLY" in y|Y|yes) CB=" --cb" ;; *) CB="" ;; esac

ask "3) Show percentage numbers / 퍼센트 숫자 표시? [y/N]:" n
case "$REPLY" in y|Y|yes) PCT=" --pct" ;; *) PCT="" ;; esac

FLAGS="$BG$CB$PCT"
CMD="$CMD_BIN $FLAGS"

echo
echo "Preview / 미리보기:"
echo
# shellcheck disable=SC2086
echo "$SAMPLE" | "$BIN" $FLAGS
echo
echo "statusLine command: $CMD"
echo

if [ -f "$SETTINGS" ] && grep -qs '"statusLine"' "$SETTINGS"; then
    echo "Note / 참고: $SETTINGS already has a statusLine entry; it will be replaced."
fi
ask "Write it to $SETTINGS? / 저장할까요? [Y/n]:" y
case "$REPLY" in n|N|no) echo "Not saved."; manual "$CMD"; exit 0 ;; esac

if ! has_python; then
    echo "python3 not found, so settings.json was not edited."
    manual "$CMD"
    exit 0
fi

had_settings=0; [ -f "$SETTINGS" ] && had_settings=1
settings write "$CMD" || { manual "$CMD"; exit 1; }
echo "Saved / 저장했어요.$( [ $had_settings = 1 ] && echo " (backup: $SETTINGS.bak)")"
echo "Run 'make setup' any time to change it / 바꾸려면 언제든 'make setup'."
