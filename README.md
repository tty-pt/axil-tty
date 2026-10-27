<p align="center">
  <img src="assets/logo.png" alt="axil-tty logo" width="220" />
</p>

# axil-tty

[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](#)
[![PTY terminals](https://img.shields.io/badge/browser-PTY-4B8BBE)](#)

> WebSocket-to-PTY bridge module for [axil](https://github.com/tty-pt/axil) — interactive browser terminal sessions over a login shell. Canonical header is `<ttypt/axil-tty.h>`.

<p align="center">
  <img src="https://github.com/tty-pt/axil-tty/blob/main/usage.gif?raw=true" width="512" />
</p>

---

## 1. Quickstart

Start `axil` with the terminal bridge loaded:

```bash
axil -d -A -p 8080 -m axil-tty
```

Open `http://localhost:8080/tty` in your browser to access an interactive terminal connected to a login shell.

### CLI Options (Inherited from axil)
| Option | Argument | Description |
| :--- | :--- | :--- |
| `-m` | `axil-tty` | Load the terminal bridge module |
| `-p` | `PORT` | HTTP/WS listen port (default: `8080`) |
| `-A` | *none* | Auto-authenticate all connections (anonymous mode) |
| `-d` | *none* | Run in foreground (debug mode) |

---

## 2. Security & Terminal Gating

When deployed in authenticated environments alongside [axil-auth](https://github.com/tty-pt/axil-auth):

1. **Session Verification**: The `/tty` WebSocket endpoint validates the `QSESSION` cookie against in-memory corm session tables.
2. **Login Shell Check**: `axil-tty` inspects `getpwnam()` for the authenticated user's account.
3. **Fail-Closed Gating**: If the account's shell is set to `/bin/false` or `/sbin/nologin`, connection is rejected with HTTP 403 Forbidden before allocating a PTY. Only authorized system users with a valid POSIX shell (`/bin/sh`, `/bin/bash`) can establish interactive sessions.

---

## 3. Browser Terminal Integration (NPM)

### Install via NPM
Install `@tty-pt/axil-tty` alongside the required peer dependencies:

```bash
# In your web project:
npm install @tty-pt/axil-tty @xterm/xterm @xterm/addon-fit

# Or install globally for the terminal CLI:
npm install -g @tty-pt/axil-tty
```

### Embed in Any Web Application

```javascript
import { create } from "@tty-pt/axil-tty";

const term = create(document.getElementById("terminal"), {
  url: `ws://${location.hostname}:${location.port}/tty`,
  sub: {
    cols: 80,
    rows: 24,
    onOpen: () => console.log("Connected to terminal server"),
    onClose: () => console.log("Disconnected"),
  }
});
```

---

## 4. C API Integration

```c
#include <ttypt/axil-tty.h>

// Spawn a login shell on a WebSocket connection
call_axil_tty_shell(fd);

// Spawn a specific command over PTY
char *argv[] = { "/bin/bash", NULL };
call_axil_tty_exec(fd, argv);

// Check if connection is managed by this module
if (call_axil_tty_owns(fd)) {
    // Handled by axil-tty
}
```

---

## 5. Installation & Build

### Prebuilt Packages
Prebuilt packages are distributed on tty.pt for Linux (APT / Alpine / Arch / Fedora-RHEL), macOS (Homebrew), Windows (winget / MSYS2), and OpenBSD. Follow the [installation instructions](https://github.com/tty-pt/ci/blob/main/docs/install.md) and use **libaxil-tty** as the package name.

### Build from Source
```sh
make                  # builds lib/libaxil-tty.so
make test             # run in-tree test suite
sudo make install     # install module and htdocs to system
```

## License

BSD 2-Clause License. Copyright (c) 2026, tty-pt. See `LICENSE`.
