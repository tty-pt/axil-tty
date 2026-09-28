## 1.2.0

- **Fixed the arm64 macOS (Homebrew) build.** The PTY child was spawned with
  `execvpe(3)`, a glibc extension (in POSIX.1-2008 TC1, but never implemented on
  Darwin) that only ever compiled because `_GNU_SOURCE` makes glibc declare it.
  Darwin ignores that macro, and clang ≥15 treats the resulting implicit
  declaration as a hard error, so `brew`'s arm64 job could not build the module
  at all. Replaced with plain `execve(3)` — carrying the curated environment,
  with `execvp(3)` kept as the fallback — matching axil's own convention that
  `argv[0]` is a full path (as in `axil_posix` `popen2`). No `PATH` search
  machinery is needed, as every caller passes an absolute path.
- **The spawned shell now gets the environment we intended on macOS.** Since
  `execvpe` was missing there, Darwin fell through to `execvp` and silently
  discarded the whole `env` array, inheriting the server's environment instead.
  The child environment is now platform-aware: `PATH` prepends Homebrew's arm64
  prefix (`/opt/homebrew`, since `/usr/local` is Intel's), and
  `LD_LIBRARY_PATH` becomes `DYLD_LIBRARY_PATH` (best-effort — SIP strips it
  for SIP-protected binaries). Linux and the BSDs are unchanged.
- **Fixed a fatal defect: standalone `GET:/tty` delivered no data.** The handler
  upgraded the socket but never called `axil_fd_watch()`. axil deliberately
  never routes WebSocket frames through `on_axil_parse`/`cmd_parse` — frames
  reach a module only via `axil_ws_read()` after it watches the fd — so the
  route negotiated at the transport layer and then never saw telnet options,
  NAWS, or PTY input. It only ever worked under axil-nd, which watches and
  reads the fd itself.
- **Each connection now has exactly one reader.** `handle_tty` watches the
  client fd, `on_axil_tick` drains it with `axil_ws_read()` and feeds
  `axil_tty_input()`, and only for connections this module owns. Ownership is
  recorded in `mux_state.owns_client` and exposed as the new
  **`axil_tty_owns(fd)`** predicate, which axil-nd checks so that two loaded
  modules cannot split one socket's frame stream between their ticks.
- **NAWS geometry no longer requires a live PTY.** A client negotiates its
  window size at socket-open, long before any command exists, so the size is
  now recorded in a dedicated `fd → winsize` map that is independent of
  per-connection state; `axil_tty_exec()` seeds a new PTY from it. Entries are
  dropped on disconnect *before* the state lookup, since fd numbers are reused.
  `axil_tty_input()` also bounds-checks truncated telnet options.
- **`on_axil_connect` only claims `GET:/tty`** (`AXIL_TTY_ROUTE`, the same
  literal the route is registered with, so the two cannot drift). A connection
  on another route gets no negotiation, no `O_NONBLOCK`, no PTY and no state.
  Note this is defensive rather than load-bearing: loaded as a `DT_NEEDED`
  dependency of `libaxil-nd.so`, axil dispatches `on_axil_connect` to the
  primary module only, which was verified with an instrumented build.
- **`create()` merges a partial `sub` over the defaults** (`Object.assign`)
  instead of substituting it, so passing only some hooks no longer throws on
  `sub.onOpen` being undefined. The returned instance is now a copy, so
  mutating it no longer writes through to the caller's object.
- `onMessage` is now optional in `types/axil-tty.d.ts`, and the stale `/terminal`
  default in the type docs is corrected to `/tty`.
- **Renamed `ndc-tty` → `axil-tty`**: the module now integrates with the axil
  HTTP/server library and its xy hooks; headers and build targets renamed
  accordingly.
- **`axil_tty_exec(fd, argv)` / `axil_tty_shell(fd)`** — replaces
  `ndc_tty_exec`/`ndc_tty_shell`; opens a PTY and execs `argv`
  (`argv[0] == NULL` uses the user's login shell).
- **Module artifact**: `libaxil-tty.so` (with a `lib/axil-tty.so` link), loaded
  via `axil -m axil-tty`; the module registers the `GET:/tty` WebSocket
  upgrade + PTY handler.
- **NPM package**: `ndc-tty` → `@tty-pt/libaxil-tty` (`axil-tty.js`/`axil-tty.css`/
  `demo.js`, `axil-tty-cli.js` bin, `types/axil-tty.d.ts`).
- **JS assets renamed off `axil`** — `axil` is the HTTP server, not this module,
  so nothing on the JS side is named after it: the library `axil.js` →
  `axil-tty.js`, the stylesheet `axil.css` → `axil-tty.css`, the page bootstrap
  `axil-tty.js` → `demo.js`, and the CLI shim `axil-cli.js` → `axil-tty-cli.js`
  (the installed `axil` command is unchanged). The C handlers now serve
  `GET:/axil-tty.js`, `GET:/axil-tty.css`, and `GET:/demo.js`.
- **Fixed** the duplicate `create` export in the library that broke
  `bun run build`.
- **Default WebSocket URL is now `/tty`** (was `/terminal`), matching the route
  the module actually serves; consumers no longer need to pass `url`.
- **New** `types/axil-tty.d.ts`; the `types` condition in `exports` now comes
  first so TypeScript resolves it. `types` is no longer gitignored.
- **Dropped** the `module` field: it pointed at `esm/axil-tty.js`, which the
  build never emitted (the esm target globs `src/`, which is C-only). ESM
  consumers use `exports["."].import`.
- **Fixed** `test.sh`: the handshake request was built with `$(printf …)` and
  bash stripped its trailing newline, leaving the request unterminated, so the
  server never replied and the suite always failed with "axil did not become
  ready". Asset-route checks added.
- **Fixed** the `AXIL_HTDOCS` asset-root override, which was read from the
  per-connection request environment and so never saw the server's own
  environment; it is now read with `getenv()`.
- **Removed** the unused `axil_tty_pw_free()` (the only build warning).
- **Known limitation**: over a WebSocket, axil never runs `cmd_parse`, so the
  registered `sh` command cannot be reached by name. Standalone `/tty` starts
  its shell from the first NAWS instead. axil-nd is unaffected — its `sh` runs
  from its own command pipeline.

---

## [1.0.1] - 2026-04-12

- TTY/PTY functionality extracted from ndc core into this standalone module.
- **New:** `ndc_tty_exec(fd, argv)` — replaces `ndc_pty(fd, args)` from ndc
  core; opens a PTY and execs `argv` on `fd`. `argv[0] == NULL` uses the
  user's login shell.
- **New:** `ndc_tty_shell(fd)` — replaces `do_sh`; opens a PTY login shell on
  `fd`.
