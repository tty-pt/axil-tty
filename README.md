# axil-tty

[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](#)
[![PTY terminals](https://img.shields.io/badge/browser-PTY-4B8BBE)](#)

> WebSocket-to-PTY bridge module for axil — browser terminal sessions over a login shell.

<img src="https://github.com/tty-pt/axil-tty/blob/main/usage.gif?raw=true" width="512" />

A dynamic module for [axil](https://github.com/tty-pt/axil) that adds:

- WebSocket-to-PTY bridging (browser terminal sessions)
- Telnet negotiation (NAWS window resize, ECHO, SGA)
- Browser terminal JS/CSS assets (`axil.js`, `axil.css`, `axil-tty.js`)
- Login-shell `sh` handler — spawns a shell over a WebSocket

## Contents

- [Features](#features)
- [Install](#install)
- [Build from source](#build-from-source)
- [Quick start](#quick-start)
- [Browser terminal](#browser-terminal)
- [C API](#c-api)
- [Testing](#testing)
- [Documentation](#documentation)
- [License](#license)

## Features

- **WebSocket↔PTY bridging** — browser terminal sessions wired to a PTY on
  the server.
- **Telnet negotiation** — NAWS window resize, ECHO, and SGA for a proper
  terminal experience.
- **Login-shell handler** — an `sh` command spawns a login shell over the
  WebSocket, via `axil_tty_shell`/`axil_tty_exec`.
- **Browser assets** — `axil.js`, `axil.css`, and `axil-tty.js` served from
  `htdocs/` (installed under `share/axil/htdocs`).

## Install

Prebuilt packages are distributed from [tty.pt](https://tty.pt) for Linux (APT
/ Alpine / Arch / Fedora-RHEL), macOS (Homebrew), Windows (winget / MSYS2), and
OpenBSD. Follow the [installation instructions]
(https://github.com/tty-pt/ci/blob/main/docs/install.md) and use **libaxil-tty**
as the package name. The module installs as `libaxil-tty.so` in the axil module
directory, and the NPM terminal widget is published as `@tty-pt/libaxil-tty`.

## Build from source

The module builds with a plain `make` (the shared [`mk` include.mk]
(https://github.com/tty-pt/mk) is expected as a sibling directory):

```sh
make                  # builds lib/libaxil-tty.so (and the lib/axil-tty.so link)
make test             # run the in-tree test suite (./test.sh)
sudo make install     # module + htdocs → $(PREFIX), default /usr/local
```

**Dependencies:** `libaxil`, `libcorm`, `libxylem` packages (from the tty.pt
repo) provide the headers and libraries.

## Quick start

Start axil with the module loaded:

```sh
axil -d -A -p 8080 -m axil-tty
# run from the repo's lib/ so `-m axil-tty` resolves, or pass the installed
# module path, e.g. -m axil-tty once installed, or -m /path/to/lib/axil-tty
```

If the `libaxil-tty` package is installed, `-m axil-tty` resolves from
anywhere (the module lives in the axil module directory); the working-directory
hint above only matters when running from a source checkout.

### Command-line options (inherited from axil)

| Option | Description |
|--------|-------------|
| `-m PATH` | Load module from PATH (colon-separated list) |
| `-A` | Auto-authenticate all connections |
| `-p PORT` | HTTP/WS listen port |
| `-C PATH` | Change directory before starting |
| `-d` | Don't detach (run in foreground) |

Open `http://localhost:8080` for a terminal connected to a login shell.

## Browser terminal

Install the NPM package:

```sh
npm install @tty-pt/libaxil-tty
```

JavaScript/TypeScript API:

```js
import { create } from "@tty-pt/libaxil-tty";

// Create terminal instance
const term = create(document.getElementById("terminal"), {
  proto: "ws",        // or "wss" for secure
  port: 4201,
  sub: {
    onOpen: (term, ws) => {
      console.log("Connected to server");
    },
    onClose: (ws) => {
      console.log("Disconnected, reconnecting...");
    },
    onMessage: (ev, arr) => {
      // Return true to continue default processing
      return true;
    },
    cols: 80,
    rows: 25,
  },
  debug: false,
});
```

See `types/axil.d.ts` for full TypeScript definitions.

## C API

```c
#include <ttypt/axil-tty.h>

// Spawn a login shell on a WebSocket connection
call_axil_tty_shell(fd);

// Spawn a specific command
char *argv[] = { "/bin/bash", NULL };
call_axil_tty_exec(fd, argv);
```

### WebSocket upgrade

axil does **not** auto-upgrade WebSocket requests. Modules must opt in
explicitly. axil-tty registers a `GET:/tty` handler that detects the upgrade
and calls `axil_ws_upgrade(fd)`:

```c
static int
handle_tty(socket_t fd, char *body)
{
    char key[ENV_VALUE_LEN] = {0};
    if (axil_env_get(fd, key, "HTTP_SEC_WEBSOCKET_KEY") == 0) {
        axil_ws_upgrade(fd);  /* performs handshake, calls axil_connect() */
        return 0;
    }
    serve_htdocs(fd, "index.html");
    return 0;
}
```

After `axil_ws_upgrade` succeeds, `axil_connect()` fires — axil-tty's
`on_axil_connect` handler sets up the PTY and Telnet negotiation.

## Testing

```sh
./test.sh             # or: make test
```

The suite boots axil with the module on a random port and verifies the
WebSocket handshake (computed `Sec-WebSocket-Accept`), the telnet IAC
negotiation (`DO NAWS`, `WILL ECHO`, `WONT SGA`), and that PTY output
(`echo AXIL_TEST`) streams back over the socket.

## Documentation

- Man pages: `man axil_tty_shell` and `man axil_tty_exec` (generated by
  `make docs` from the Doxygen-annotated header).
- API: [`include/ttypt/axil-tty.h`](./include/ttypt/axil-tty.h)
- Version history: [`CHANGELOG.md`](./CHANGELOG.md)

## License

BSD 2-Clause License. Copyright (c) 2026, tty-pt. See `LICENSE`.