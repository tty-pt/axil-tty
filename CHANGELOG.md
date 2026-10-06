## 1.3.2

- **Fail closed on unauthenticated or automatic `-A` terminal access.** Who may
  have a terminal is now resolved fresh for every request by `tty_identity()`,
  and it is deliberately **not** `axil_get_pw()`: that call substitutes the
  server's own entry for any unauthenticated descriptor, and three call sites
  used to treat the substitution as the caller's identity. Now an
  unauthenticated descriptor, a `-A`-published identity (`DF_AUTH_AUTO`), and a
  name with no passwd entry all resolve to NULL — no identity, no terminal.
  `tty_no_shell()` refuses the known no-op shells (`false`, `nologin`,
  `/bin/false`, `/usr/bin/false`, `/sbin/nologin`, `/usr/sbin/nologin`) and
  counts anything unrecognised as a real shell: it is a deny-list on purpose,
  because the failure that must never happen is a no-shell account treated as
  shellable. `tty_refuse()` answers with one log line and one client line —
  the text travels as a WebSocket frame, so even a browser sees why before the
  close — and there is no PTY, no fork. `drop_priviledges()` now takes the
  resolved identity and refuses the child when there is none, and
  `mux_init()` tolerates a chroot with no `/etc/passwd` (empty `mux_pw`,
  nothing dereferences it).
- **Generation-validated mux state; HTTP bypass (S5.4/S5.5).** `mux_state` and
  the NAWS entry carry the connection generation and `mux_get()`/`mux_wsz_get()`
  refuse a mismatch, so a leaked entry is inert on a recycled fd instead of
  handing the next connection a live shell. Chunks that open with an HTTP
  request line skip telnet processing entirely — a `0xFF` body byte used to be
  read as IAC, which slid the request head off the input and answered the POST
  with the telnet banner (shell bytes ride WebSocket frames and still reach the
  PTY, so the exemption is only for the HTTP case). `AXIL_TTY_TRACE` logs PTY
  handover instead of doing it silently, and the build now uses the in-tree
  axil headers.
- **Tests**: `test.sh` asserts an unauthenticated `/tty` yields no PTY, no
  child and no output — only a refusal line, a close and a log entry — and
  keeps the retained regression that an authenticated PTY connection killed
  abruptly still cleans up.

## [1.3.1]

- **`axil_tty_handle_tty(cfd, body)` is exported** (it was the static
  `handle_tty`): a host module that embeds this one — axil-nd — can register
  the `GET:/tty` route itself instead of depending on the nested `xy_load()`
  that installed it as a side effect. `axil_register_handler()` is last-wins,
  so registering from both places is safe.

## [1.3.0]

- **Opt-in line mode for routes with no line discipline (`lineMode`).** A game
  socket says `WONT ECHO` and runs no PTY, so per-keystroke frames used to hit
  the game command parser one character at a time (and a bare Return died in
  `cmd_new`). With `lineMode: true`, the client holds the line while the server
  says `WONT ECHO` and submits one `"<line>\n"` frame per Enter -- the same
  shape `sendCmd()` uses -- with local echo, code-point-aware erase, pasted
  CRLF split into one submit per line, and escape-led input swallowed. The
  moment the server says `WILL ECHO` (a PTY is born: `sh`, `man`, `/tty`) the
  branch goes inert and every keystroke reaches the driver immediately, so
  there is still exactly one echoer in both states. Off by default; `/tty`
  behaviour is unchanged.

## [1.2.1]

- **Reverted `b0485b0`'s second hunk: `sh` spawns the login shell again.**
  That commit meant to harden the empty-passwd fallback but changed
  `axil_tty_shell` from `{NULL, NULL}` to `{"/bin/sh", NULL}`, which bypasses
  `command_pty()`'s whole `pw_shell → mux_pw → /bin/sh` chain — every `sh`
  landed in dash, a canonical-mode reader with no line editing, where arrow
  keys arrived as raw escape bytes (cursor jumps plus garbage in the buffer,
  since `ECHOCTL` stays off). Passing NULL restores the login shell
  (`/bin/bash` here, readline, working history) while keeping the first
  hunk's empty-field hardening intact.

- **Fixed: nothing was echoed as you typed.** 1.2.0 (`66631d2`, "axil-nd
  compat") rewrote the browser client to impersonate a line discipline: buffer
  every keystroke into `term.inputBuf`, send the line only at Enter, and echo
  locally only when the server had said `WONT ECHO`. But this module has always
  negotiated `WILL ECHO` and created PTYs with the OS default `c_lflag`, which
  has `ECHO` set -- so the local-echo branch was unreachable *and* the client was
  withholding the bytes until Enter, leaving the driver nothing to echo in the
  meantime. The screen stayed blank until the line was submitted. The client is a
  pipe again: every keystroke goes out as it is typed, and the line discipline
  does the echoing, line editing, `^?` erase and CR/LF translation, which is
  what a real terminal is.
- The three PTY creation sites now share one `mux_pty_termios()` helper, which
  also closes a real gap: `mux_ensure()` never touched `c_oflag` at all, and that
  is the path a raw-telnet `sh` guest takes, so its output arrived as bare LFs --
  the staircase you get when a terminal steps down a line without returning to
  column 0. The helper sets `ICRNL`, clears `IGNCR`/`INLCR`, sets
  `OPOST`/`ONLCR` and clears `OCRNL` everywhere.
- **One echo owner per session, stated at connect and never revisited.**
  `WILL ECHO` goes out on connect and again after the child exits, and that is
  the only time the policy is stated. `axil_tty_update()` is gone: it re-read the
  lflag on every PTY output and told the client `WONT ECHO` the moment a program
  cleared `ECHO`, which for any client that honours it means local echo landing
  on top of a program that is about to render the line itself -- vim would show
  every keystroke twice and its cursor would drift from the real one. Two echoers
  is the one failure nobody can recover from, so there is never a second one.
  This is also why the helper leaves `ECHO` alone: bash's readline only echoes at
  all when it inherits `ECHO` on, and it takes the tty out of canonical mode to
  do its own line editing, so `sh`, `bash` and `vim` each end up with exactly one
  echoer between them.
- Accepted trade-off: a program that clears `ECHO` and then expects its *peer* to
  echo for it will show nothing. That set is smaller than the `bash` case above,
  it is what 1.1.x did, and no real terminal has that property either.
- The suite now asserts `WILL ECHO` (and the absence of `WONT ECHO`/`WILL SGA`),
  that a **partial** line is echoed back before Enter -- the user-visible feature
  stated directly as a test, which fails under any policy where the client
  withholds bytes -- and that PTY output arrives CRLF-terminated.

## [1.2.0]

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
