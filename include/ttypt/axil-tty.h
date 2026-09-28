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

/** Open a PTY and exec argv on fd. argv[0]==NULL uses the user's login shell.
 *
 *  argv[0] should be a full path, as with axil's own popen2(); the child is run
 *  with this module's curated environment (HOME, USER, SHELL, TERM, ...). A
 *  bare name still resolves, but via the server's own PATH and without that
 *  environment. */
XY_DECL(int, axil_tty_exec, socket_t, fd, char **, argv);

/** Open a PTY shell (login shell) on fd. */
XY_DECL(int, axil_tty_shell, socket_t, fd);

/** Check if a PTY child process is active on fd. Returns 1 if active, 0 otherwise. */
XY_DECL(int, axil_tty_active, socket_t, fd);

/** Check if this module owns the WebSocket on fd, i.e. the client connected to
 *  our own `GET:/tty` route. Returns 1 if owned, 0 otherwise.
 *
 *  Ownership here means the route, not the reading. axil decodes WebSocket
 *  frames and delivers each payload to on_axil_parse, which is
 *  axil_tty_input() -- for our own route and for a borrowed socket alike. No
 *  module needs axil_fd_watch() to receive client bytes, and a module that
 *  claims a client socket with axil_fd_watch() stops receiving them: the flag
 *  takes the descriptor out of descr_read() and gives it to axil_fd_tick()
 *  instead. */
XY_DECL(int, axil_tty_owns, socket_t, fd);

/** Negotiate terminal options on a client socket this module does not own.
 *
 *  Creates the per-connection state if absent and sends the same negotiation
 *  our own route sends on connect: `WILL ECHO` (once per socket, guarded so a
 *  later axil_tty_exec()/command_pty() is not a second statement of the same
 *  policy), `WONT SGA`, and `DO NAWS`. Returns 0, or -1 if the state could not
 *  be created.
 *
 *  No PTY is opened and no descriptor is claimed -- this is negotiation only.
 *  Use it from on_axil_connect() so a socket shared with a host module (axil-nd
 *  runs its game and its shell on one /nd socket) gets a real terminal
 *  negotiation and a window size, instead of the host reimplementing the
 *  telnet options. The PTY, and the `WILL ECHO` that belongs to it, still come
 *  from axil_tty_shell()/axil_tty_exec(). */
XY_DECL(int, axil_tty_attach, socket_t, fd);

/** Feed client input to the PTY bridge on fd, consuming any telnet options.
 *
 *  Consumes IAC sequences (NAWS updates the window size and may auto-spawn the
 *  shell, DO/DONT/WILL are dropped) and writes whatever follows to the PTY.
 *  This is the body of the on_axil_parse hook, exported for callers that
 *  receive client bytes by some other route.
 *
 *  Returns -1 when a PTY child consumed the input (caller must not also treat
 *  it as a command), otherwise the number of leading bytes consumed as telnet
 *  options, which is 0 when the whole buffer is PTY input.
 *  `input` is not modified. */
XY_DECL(int, axil_tty_input, socket_t, fd, unsigned char *, input, int, nread);

#endif