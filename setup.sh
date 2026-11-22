#!/bin/sh
# Point Claude Code's settings.json at the status line (a .bak copy is kept).
#   make copy       -> setup.sh copy      (installs the binary)
#   make install    -> setup.sh install   (asks before replacing another status line)
#   make uninstall  -> setup.sh remove    (removes the binary and entry, if ours)
set -e
umask 077
export LC_ALL=C # treat paths as bytes: other locales garble UTF-8 in sh and sed

die() { printf 'setup.sh: %s\n' "$*" >&2; exit 1; }

DIR="${CLAUDE_CONFIG_DIR:-${HOME:+$HOME/.claude}}"
case "$DIR" in
*[[:cntrl:]]*) die "the config path contains a control character." ;;
/*) ;;
*) die "HOME or CLAUDE_CONFIG_DIR must be an absolute path (got '$DIR')." ;;
esac
if [ "$(id -u)" = 0 ] && [ "${SUDO_USER:-root}" != root ]; then
    die "do not use sudo; run make as $SUDO_USER."
fi
BIN="$DIR/statusline"
SETTINGS="$DIR/settings.json"
# shellcheck disable=SC2088 # Claude Code expands the ~
if [ "$DIR" = "$HOME/.claude" ]; then
    CMD="~/.claude/statusline"
else
    # the command is run by a shell: single-quote it so nothing expands
    CMD="'$(printf '%s' "$BIN" | sed "s/'/'\\\\''/g")'"
fi

# A compiled binary with strings common to every version of this status line.
our_binary() {
    [ -f "$BIN" ] && [ ! -L "$BIN" ] || return 1
    case "$(od -An -tx1 -N4 "$BIN" | tr -d ' \n')" in
    7f454c46|cffaedfe|cafebabe|4d5a*) ;;
    *) return 1 ;;
    esac
    for mark in five_hour seven_day '[38;5;240m'; do
        grep -qF -- "$mark" "$BIN" || return 1
    done
}

manual() {
    echo "Add this to $SETTINGS yourself / 직접 추가해 주세요:"
    printf '  "statusLine": { "type": "command", "command": "%s", "refreshInterval": 60 }\n' \
        "$(printf '%s' "$CMD" | sed 's/["\\]/\\&/g')"
}

# settings.json helper: check | exists | write CMD | remove
# Exit status: 0 = yes or done, 3 = no, anything else = error.
settings() {
    # -X utf8: read the paths as UTF-8 whatever the locale
    python3 -I -X utf8 - "$SETTINGS" "$BIN" "$@" <<'EOF'
import json, math, os, re, shlex, sys, tempfile
path, binary, action = sys.argv[1], sys.argv[2], sys.argv[3]
real = os.path.realpath(path)  # edit what a dotfile symlink points at

def load():
    if not os.path.lexists(path):
        return None
    if not os.path.isfile(path):
        sys.exit(f"{path} is not a file or is a broken link; not edited.")
    with open(path, "rb") as f:
        return f.read()

def unique(pairs):
    if len({k for k, _ in pairs}) != len(pairs):
        raise ValueError("duplicate key")
    return dict(pairs)

def finite(s):
    n = float(s)
    if not math.isfinite(n):
        raise ValueError("number out of range")
    return n

try:
    raw = load()
    text = (raw or b"").decode("utf-8")
    data = json.loads(text, object_pairs_hook=unique, parse_float=finite,
                      parse_constant=finite) if text.strip() else {}
except (OSError, ValueError, RecursionError) as e:
    sys.exit(f"cannot safely edit {path} ({e}); not edited.")
if not isinstance(data, dict):
    sys.exit(f"{path} is not a JSON object; not edited.")

def ours():  # does statusLine.command run this binary?
    sl = data.get("statusLine")
    cmd = sl.get("command") if isinstance(sl, dict) else None
    try:
        exe, *args = shlex.split(cmd) if isinstance(cmd, str) else [""]
    except ValueError:  # bad quoting, or blank
        return False
    # only plain options may follow: "statusline; other" is the user's own command
    if not exe or "\n" in cmd:
        return False
    if not all(re.fullmatch(r"[\w=-]+", a) for a in args):
        return False
    if not cmd.lstrip().startswith("'"):  # unless single-quoted, the shell expands it
        exe = os.path.expandvars(exe)
        if "$" in exe or "`" in exe:  # checked before ~, as HOME itself may contain them
            return False
        exe = os.path.expanduser(exe)
    return os.path.isabs(exe) and os.path.realpath(exe) == os.path.realpath(binary)

if action == "check":
    sys.exit(0 if ours() else 3)
if action == "exists":
    sys.exit(0 if data.get("statusLine") else 3)
if action == "remove":
    if not ours():
        sys.exit(3)
    del data["statusLine"]
else:
    if re.search("[\ud800-\udfff]", sys.argv[4]):
        sys.exit("the config path is not UTF-8, which JSON cannot hold; not edited.")
    data["statusLine"] = {"type": "command", "command": sys.argv[4], "refreshInterval": 60}

def save(dest, body, old=None, unchanged=lambda: True):
    """Private temp file in the same directory, then rename over dest."""
    fd, tmp = tempfile.mkstemp(prefix=".statusline-", dir=os.path.dirname(dest))
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(body)
            f.flush()
            if old:
                mode = old.st_mode & 0o777
                try:
                    os.fchown(f.fileno(), -1, old.st_gid)
                except OSError:
                    mode &= 0o707  # never hand the group bits to another group
                os.fchmod(f.fileno(), mode)
            os.fsync(f.fileno())
        if not unchanged():
            sys.exit(f"{path} changed meanwhile; not edited. Try again.")
        os.replace(tmp, dest)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)

try:
    body = json.dumps(data, indent=2, ensure_ascii=False) + "\n"
    old = None
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if raw is not None:
        old = os.stat(real)
        if not os.access(real, os.W_OK) or old.st_uid != os.geteuid():
            sys.exit(f"{real} is read-only or owned by another user; not edited.")
        if old.st_nlink > 1:
            sys.exit(f"{real} has other hard links; not edited.")
        save(path + ".bak", raw)  # holds secrets: always 0600
    # backslashreplace keeps a lone surrogate as its valid \\udXXX escape
    save(real, body.encode("utf-8", "backslashreplace"), old,
         lambda: load() == raw and os.path.realpath(path) == real)
except OSError as e:
    sys.exit(f"cannot write {path} ({e}); not edited.")
EOF
}

has_python() { command -v python3 >/dev/null 2>&1; }

case "${1:-install}" in
copy)
    if [ -e "$BIN" ] || [ -L "$BIN" ]; then
        our_binary || die "$BIN was not built by this project; move it away first."
    fi
    mkdir -p "$DIR"
    # Copy to a new file, then rename: macOS kills a signed binary overwritten in place.
    tmp=$(mktemp "$DIR/.statusline.XXXXXX")
    trap 'rm -f "$tmp"' EXIT
    trap 'exit 1' HUP INT TERM
    cp statusline "$tmp"
    chmod 755 "$tmp"
    mv -f "$tmp" "$BIN"
    ;;
remove)
    present=0
    if [ -e "$BIN" ] || [ -L "$BIN" ]; then
        present=1
        if ! our_binary; then
            echo "$BIN was not built by this project; nothing was removed."
            exit 0
        fi
    fi
    rc=3
    if has_python; then rc=0; settings remove || rc=$?; fi
    case "$rc" in
    0) echo "Removed the statusLine entry from $SETTINGS (backup: $SETTINGS.bak)." ;;
    3) echo "$SETTINGS was not changed." ;;
    *) die "nothing was removed." ;;
    esac
    [ "$present" = 0 ] || rm -f "$BIN"
    ;;
install)
    if ! has_python; then manual; exit 0; fi
    rc=0; settings check || rc=$?
    if [ "$rc" = 0 ]; then
        echo "Updated / 업데이트했어요: $BIN"
        exit 0
    fi
    [ "$rc" = 3 ] || { manual; exit 1; }
    rc=0; settings exists || rc=$?
    [ "$rc" = 0 ] || [ "$rc" = 3 ] || { manual; exit 1; }
    if [ "$rc" = 0 ]; then
        if [ ! -t 0 ]; then
            echo "Another status line is already set, so settings.json was not changed."
            manual
            exit 0
        fi
        printf 'Replace the current status line? / 기존 statusline을 바꿀까요? [Y/n]: '
        read -r reply || reply=n
        case "$reply" in ""|[Yy]|[Yy][Ee][Ss]) ;; *) manual; exit 0 ;; esac
    fi
    settings write "$CMD" || { manual; exit 1; }
    echo "Installed / 설치했어요: $BIN"
    ;;
*)
    echo "usage: setup.sh [copy | install | remove]" >&2
    exit 2
    ;;
esac
