# claude-statusline

**English** | [한국어](README_한국어.md)

A fast, minimal status line for [Claude Code](https://code.claude.com), written in plain C.
One ~50 KB binary with no dependencies (no `jq`, Node, or Python), and it renders in about 1–2 ms.
The colors are color-blind friendly, and severity is never shown by color alone.

```
Opus 5.5  │  ctx ━━━━━─────── 42%  │  high  ⚡fast
current  ━━━━━━━━─────   65%   ↻ 17:04
weekly   ━━━━━━━━━━━─   88%!  ↻ Thu 11/12 15:04
```

| Dark terminal | Light terminal (`--light`) |
|---|---|
| ![dark preview](docs/preview-dark.png) | ![light preview](docs/preview-light.png) |

## What it shows

| Line | Content |
|---|---|
| 1 | Model name · context window bar · reasoning effort (`low` … `max`) · `⚡fast` when fast mode is on |
| 2 | **current**: 5-hour rate-limit usage and reset time |
| 3 | **weekly**: 7-day rate-limit usage and reset date and time |

- Bars are colored by usage: blue below 50%, orange below 80%, and dark vermillion with a bold `!` marker at 80% and above. See [Color-blind friendly](#color-blind-friendly).
- The two rate-limit lines share the same layout. They appear only when Claude Code sends rate-limit data (claude.ai Pro/Max subscribers, after the first response of a session). Otherwise only line 1 is shown.
- A reset later today shows just the time. A later day adds the weekday.
- The permission mode (auto, plan, …) is not part of the status line data, and Claude Code already shows it in the footer right below.

## Install

Requires a C compiler (`cc`; on macOS run `xcode-select --install` if you don't have one).

```sh
git clone https://github.com/sjlee42/claude-statusline.git
cd claude-statusline
make install          # builds and copies the binary to ~/.claude/statusline
```

Then add this to `~/.claude/settings.json`:

```json
{
  "statusLine": {
    "type": "command",
    "command": "~/.claude/statusline"
  }
}
```

If your terminal has a light background, use `"~/.claude/statusline --light"`.

## Speed

Each run was timed 200 times on a Mac with the same sample payload. The times include process-spawn overhead from the timing harness.

| Implementation | Median | p95 |
|---|---|---|
| **claude-statusline (C)** | **2.8 ms** | 3.4 ms |
| Equivalent bash + `jq` script | 27.7 ms | 33.2 ms |

Claude Code re-runs the status line command on every update (debounced at 300 ms), so a fast start-up matters more than anything else. Runtimes launched through `npx` are the slowest case: ccstatusline's README measured about 1.1 s per repaint with `npx`.

## Does it use tokens or context?

No. The status line runs locally, and its output is only drawn on screen, never sent to the model. See the [official docs](https://code.claude.com/docs/en/statusline).

## How it works

`statusline.c` reads the JSON payload Claude Code writes to stdin and pulls out these fields with a small purpose-built JSON path lookup:

`model.display_name`, `context_window.used_percentage`, `effort.level`, `fast_mode`,
`rate_limits.five_hour.{used_percentage,resets_at}`, `rate_limits.seven_day.{used_percentage,resets_at}`

Missing or `null` fields are handled. For example, early in a session the context shows `--%`.

## Other Claude Code status lines worth a look

Before building this, we surveyed popular and recently updated status line projects on GitHub. If you want more features, themes, or a visual editor, these are great choices. Star counts were checked on 2026-11-09.

| Project | ★ | Language | Highlights |
|---|---|---|---|
| [jarrodwatts/claude-hud](https://github.com/jarrodwatts/claude-hud) | 29.1k | JavaScript (plugin) | Multi-line HUD: context, active tools, running agents, todo progress |
| [sirmalloc/ccstatusline](https://github.com/sirmalloc/ccstatusline) | 13.6k | TypeScript | Most customizable; powerline style, themes, interactive TUI editor |
| [Haleclipse/CCometixLine](https://github.com/Haleclipse/CCometixLine) | 3.5k | Rust | Fast native binary, two-line layout with usage bars and themes |
| [kamranahmedse/claude-statusline](https://github.com/kamranahmedse/claude-statusline) | 1.4k | Shell | Big, readable dot gauges for current and weekly usage (inspired this project) |
| [Owloops/claude-powerline](https://github.com/Owloops/claude-powerline) | 1.2k | TypeScript | vim-style powerline with minimal, powerline, capsule and TUI styles |
| [leeguooooo/claude-code-usage-bar](https://github.com/leeguooooo/claude-code-usage-bar) | 389 | Python | 5h/7d usage bars, reset countdowns, 3 styles × 9 themes |
| [Astro-Han/claude-pace](https://github.com/Astro-Han/claude-pace) | 245 | Bash + jq | Pace-aware quota tracking in a single file |
| [Wangnov/claude-code-statusline-pro](https://github.com/Wangnov/claude-code-statusline-pro) | 239 | Rust | Gradient context bar, Nerd Font, emoji or text modes |
| [alp82/claude-statusline](https://github.com/alp82/claude-statusline) | 26 | Bash | Context, 5h and 7d bars with per-agent rows |
| [Dworf/statusline-bar](https://github.com/Dworf/statusline-bar) | 24 | Bash + jq | Presets and themes with 49 selectable data tokens |

Also see [ccusage](https://github.com/ccusage/ccusage) (★19.4k), a usage and cost analysis tool that also offers a status line mode.

**Why another one?** We wanted the readability of the dot-gauge design, with a cleaner single style for every bar, native speed, and zero dependencies, all in a file small enough to read in one sitting.

## Color-blind friendly

The default palettes are designed for red-green color blindness (protanopia and deuteranopia), and severity never relies on color alone.

| Usage | Color | Extra cue (no color needed) |
|---|---|---|
| below 50% | blue | — |
| 50–79% | orange | — |
| 80% and above | dark vermillion | **bold** number and a `!` marker (`88%!`) |

- The filled part of a bar is a thick line (`━`) and the empty track is a thin line (`─`), so you can read how full it is even with low color contrast.
- The model name and effort use neutral colors so they are never mistaken for a gauge state.
- Every gauge also prints its exact percentage.

The image below shows the output in both palettes next to simulated protanopia and deuteranopia (Machado et al. 2009 model). Blue and orange stay clearly distinct. Orange and vermillion are told apart by brightness, bold text, and the `!` marker.

![color-blind simulation](docs/colorblind-check.png)

## License

[MIT](LICENSE)
