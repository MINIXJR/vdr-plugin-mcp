# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A VDR plugin (`libvdr-mcp.so`) that is a Model Context Protocol (MCP) server. It exposes every
SVDRP command of VDR and of the loaded plugins as an MCP tool, plus a few tools that go beyond
SVDRP (`vdr_log`, `vdr_marks`, `vdr_setmarks`, `vdr_genindex`). The README is the user
documentation; HISTORY is the changelog.

## Build

Standard VDR plugin Makefile, driven by `pkg-config vdr` (or `VDRDIR=<dir with vdr.pc>`):

    make            # builds libvdr-mcp.so
    make clean
    make install    # installs into VDR's libdir as libvdr-mcp.so.<APIVERSION>

Dependencies (Ubuntu/Debian packages): vdr-dev, nlohmann-json3-dev (header only, not via
pkg-config, must be in the default include path), libsystemd-dev, libssl-dev (OpenSSL 3).
README states VDR 2.8.4 or later, without a documented reason. On this host (Ubuntu 24.04,
yavdr VDR 2.8.1, nlohmann-json3-dev 3.11.3) 0.4.2 builds without warnings and runs
(tested 2026-11-08: tool list, a read-only call, deny list, `PLUG mcp` refusal).

There are no automated tests. The version is read by the Makefile from
`static const char *VERSION` in `mcp.cpp`.

## Packaging

master stays plain upstream source, no `debian/` here.

- Remotes: `origin` = fork MINIXJR/vdr-plugin-mcp, `upstream` = unf/vdr-plugin-mcp.
- Upstream's packaging: branch `upstream/debian/trixie` (gbp, Debian trixie).
- Local Ubuntu packaging: branch `ubuntu/noble`, branched from it, with
  `vdr-dev (>= 2.8.1~)` and `build-package.sh` (adapted from vdr-plugin-epg2vdr).
  Run `./build-package.sh` on that branch: it prompts for a changelog line, adds
  `<VERSION>-1fnu0~noble+local-N` via `dch`, builds with `dpkg-buildpackage -b` in a copy
  under `/usr/local/src/user/`, and leaves the `.deb`, `-dbgsym.ddeb` and a build log there.
- New upstream release: merge the release tag (or `upstream/debian/trixie`) into
  `ubuntu/noble`, keep the 2.8.1 Build-Depends, then run the script.

## Manual testing

The local server speaks MCP stdio framing over TCP (one JSON-RPC message per line), so it can be
driven directly on the VDR host:

    printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}}' \
                  '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' | socat - TCP:127.0.0.1:6420

Plugin-own SVDRP commands: `svdrpsend PLUG mcp KEYADD <name> ro|rw`, `KEYLIST`, `KEYDEL <name>`.
Logs: `journalctl -u vdr -f | grep mcp:`.

## Architecture

Request flow: transport -> `cMcpSession::Handle()` (JSON-RPC) -> new `cSvdrpClient` connection to
127.0.0.1:`svdrpPort` per call -> reply text back as the tool result. The plugin does **not** call
VDR internals for SVDRP tools; it is an SVDRP client of its own VDR.

- `mcp.cpp` — `cPluginMcp`: option parsing (`ProcessArgs`), starting both servers, `Active()`
  (blocks shutdown while `vdr_genindex` runs), and the plugin's own SVDRP commands for API keys.
- `config.h` — `cMcpConfig`, all settings with defaults (deny list `DEL*,HITK`, ro allow list).
  `Denied()`/`Allowed()` are implemented in `session.cpp`.
- `server.cpp` — local TCP listener (default 127.0.0.1:6420, no auth; SSH+socat provides the
  login). One `std::thread` per client, each with its own `cMcpSession`.
- `http.cpp` — "Streamable HTTP" transport (`--http`, off by default): TLS via OpenSSL, Bearer
  API key check, `Mcp-Session-Id` sessions bound to the key name, 1 h idle timeout, POST answered
  with plain application/json (no SSE), GET -> 405, requests with `Origin` -> 403.
- `keys.cpp` — `cMcpKeys` (global `McpKeys`): stores only SHA-256 hashes in
  `<configDir>/keys`; also creates the self-signed EC P-256 certificate.
- `session.cpp` — the MCP logic. `BuildTools()` runs lazily per session: parses SVDRP `HELP`
  (VDR "Topics:" block, then one "Plugin <name> ..." block per plugin), filters each command
  through `Allowed()`, and fetches `HELP <cmd>` / `PLUG <p> HELP <cmd>` as the tool description.
  Tool names are `vdr_<CMD>` / `<plugin>_<CMD>`, the single argument is `args` (raw SVDRP args).
  `Call()` flattens newlines (one command only) and refuses `vdr_PLUG mcp ...`.
- `recording.cpp` — the non-SVDRP recording tools, using VDR's API directly
  (`LOCK_RECORDINGS_READ`, `cMarks`, `cIndexFile`, `GenerateIndex()` in a `cThread`). Refuses
  recordings in use (recording, replay, cutting); only one genindex at a time.
- `journal.cpp` — `vdr_log` via sd-journal (only with `--log`; user vdr needs
  `systemd-journal` group).

## Invariants to keep

- Security filtering: SVDRP tools are filtered once in `BuildTools()`, and `tools/call` only
  dispatches to entries of that filtered `tools` list. The beyond-SVDRP tools are checked with
  `config.Allowed()` separately both when listed and in `tools/call` — a new such tool needs both. The plugin's own commands
  (`mcp` section in HELP, and `PLUG mcp` via `vdr_PLUG`) must never be reachable over MCP, so no
  client can manage API keys.
- Deny list matching: case-insensitive, trailing `*` = prefix, `PLUGIN.CMD` for plugin commands,
  beyond-SVDRP tools by tool name. `--deny` *replaces* the default list.
- VDR splits plugin arguments at blanks: `ProcessArgs` deliberately fails on stray arguments and
  on `--deny` without `=` so the deny list cannot silently lose entries (0.4.2). Keep that.
- Option changes must be mirrored in `CommandLineHelp()`, `mcp.conf` (shipped as
  `/etc/vdr/conf.avail/mcp.conf`) and the README "Options" section.
- Releases bump `VERSION` in `mcp.cpp` and add a HISTORY entry; commit subject style is
  `Version X.Y.Z: <summary>`.

## Conventions

- Code follows VDR style (2-space indent with VDR's brace/continuation layout, `cClass` names,
  `esyslog`/`isyslog`/`dsyslog` with `mcp:` prefix). Unlike classic VDR code this plugin uses
  `std::string`, `std::vector`, `std::thread`, `std::mutex` and nlohmann::json throughout — match
  the surrounding code rather than converting to `cString`/`cList`.
- Code, comments, README and HISTORY are in English; older commit messages are German, newer
  ones English.
