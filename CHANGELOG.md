## 1.1.0

- **Renamed `ndc-tty` → `axil-tty`**: the module now integrates with the axil
  HTTP/server library and its xy hooks; headers and build targets renamed
  accordingly.
- **`axil_tty_exec(fd, argv)` / `axil_tty_shell(fd)`** — replaces
  `ndc_tty_exec`/`ndc_tty_shell`; opens a PTY and execs `argv`
  (`argv[0] == NULL` uses the user's login shell).
- **Module artifact**: `libaxil-tty.so` (with a `lib/axil-tty.so` link), loaded
  via `axil -m axil-tty`; the module registers the `GET:/tty` WebSocket
  upgrade + PTY handler.
- **NPM package**: `ndc-tty` → `@tty-pt/libaxil-tty` (`axil.js`/`axil.css`/
  `axil-tty.js`, `axil-cli.js` bin, `types/axil.d.ts`).

---

## [1.0.1] - 2026-04-12

- TTY/PTY functionality extracted from ndc core into this standalone module.
- **New:** `ndc_tty_exec(fd, argv)` — replaces `ndc_pty(fd, args)` from ndc
  core; opens a PTY and execs `argv` on `fd`. `argv[0] == NULL` uses the
  user's login shell.
- **New:** `ndc_tty_shell(fd)` — replaces `do_sh`; opens a PTY login shell on
  `fd`.