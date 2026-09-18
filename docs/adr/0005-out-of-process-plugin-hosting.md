# ADR 0005 — Out-of-process VST3/CLAP plugin hosting

**Status:** Accepted · 2026-09-18

## Context
Plugin hosting lets users bring tools we will never build, and is a Pro-tier
feature. Third-party plugins are also, empirically, unstable — they crash, leak,
and occasionally hang.

## Decision
Host plugins in **separate processes** (`sa-plughost.exe`, one per vendor),
with audio over shared memory and control over a named pipe.

## Rationale
- A plugin crash must never destroy a user's unsaved work. In-process hosting
  makes every installed plugin a liability for our stability reputation — and
  users blame the host, not the plugin.
- The host process is restartable and its state reconstructable, so a crash
  becomes a brief interruption rather than data loss.
- The same sandbox mechanism can later isolate untrusted file parsers, which are
  a known CVE surface.

## Consequences
- Roughly two weeks of additional engineering versus in-process hosting.
- One buffer of round-trip latency — acceptable offline, tolerable for monitoring.
- Plugin GUI embedding across a process boundary is fiddly on Windows and needs
  care.

## Revisit if
Latency proves unacceptable for live monitoring workflows. Even then, prefer an
opt-in in-process "low latency, less safe" mode over changing the default.
