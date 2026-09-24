#ifndef AXIL_TTY_H
#define AXIL_TTY_H

/**
 * @file axil-tty.h
 * @brief WebSocket-to-PTY bridge module for axil.
 *
 * Bridges browser terminal sessions (WebSocket) to PTYs on the server:
 * telnet negotiation (NAWS, ECHO, SGA), a login-shell `sh` handler, and
 * browser terminal assets. Registers a `GET:/tty` upgrade handler; callers
 * dispatch through the xy bus.
 */

#include <ttypt/axil.h>
#include <ttypt/axil-xy.h>

/** Open a PTY and exec argv on fd. argv[0]==NULL uses the user's login shell. */
XY_DECL(int, axil_tty_exec, socket_t, fd, char **, argv);

/** Open a PTY shell (login shell) on fd. */
XY_DECL(int, axil_tty_shell, socket_t, fd);

#endif