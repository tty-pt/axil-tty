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
 *  axil never routes WebSocket frames through on_axil_parse, so a module that
 *  wants the bytes must call axil_fd_watch() and read them with axil_ws_read()
 *  from its on_axil_tick. When we own the connection we do that ourselves, so a
 *  host module that also borrows this bridge (axil-nd) must skip its own read
 *  for such fds -- otherwise the two ticks split the frame stream. */
XY_DECL(int, axil_tty_owns, socket_t, fd);

/** Feed client input to the PTY bridge on fd, consuming any telnet options.
 *
 *  Consumes IAC sequences (NAWS updates the window size and may auto-spawn the
 *  shell, DO/DONT/WILL are dropped) and writes whatever follows to the PTY.
 *  This is the body of the on_axil_parse hook, exported for callers that
 *  receive client bytes by some other route -- notably a module's
 *  axil_fd_tick(), since axil no longer routes WebSocket frames through
 *  on_axil_parse.
 *
 *  Returns -1 when a PTY child consumed the input (caller must not also treat
 *  it as a command), otherwise the number of leading bytes consumed as telnet
 *  options, which is 0 when the whole buffer is PTY input.
 *  `input` is not modified. */
XY_DECL(int, axil_tty_input, socket_t, fd, unsigned char *, input, int, nread);

#endif