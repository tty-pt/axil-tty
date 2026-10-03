#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
#define _GNU_SOURCE 1

/* xy-mod.h must come first so it defines the module xy context used by the
 * XY_CALL dispatches in this translation unit */
#include <ttypt/xy-mod.h>
#include <ttypt/axil.h>
#include <ttypt/corm.h>
#include <ttypt/qsys.h>

#include <arpa/telnet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#if defined(__APPLE__) || defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__)
#include <util.h>
#else
#include <pty.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* Explicit declarations for functions hidden by _XOPEN_SOURCE */
#ifdef __APPLE__
int setgroups(int, const gid_t *);
int initgroups(const char *, int);
#endif
#ifdef __OpenBSD__
int setgroups(int, const gid_t *);
int initgroups(const char *, gid_t);
#endif

#ifndef AXIL_PREFIX
#define AXIL_PREFIX "/usr/local"
#endif

#ifndef AXIL_HTDOCS
#define AXIL_HTDOCS AXIL_PREFIX "/share/axil/htdocs"
#endif

/* The one route this module owns. on_axil_connect() claims a connection only
 * when the request path matches this, so that when axil-nd does xy_load() on
 * us we never touch its sockets: no telnet negotiation, no O_NONBLOCK, no
 * PTY, no per-connection state. Keep this and the axil_register_handler() call
 * in xy_install() in sync. */
#define AXIL_TTY_ROUTE "/tty"

/* OpenBSD may not define ECHOCTL */
#ifndef ECHOCTL
#define ECHOCTL 0
#endif

/* ------------------------------------------------------------------ */
/* Per-connection PTY state stored in the module corm maps             */
/* ------------------------------------------------------------------ */

/* Who is echoing on a socket right now. ECHO is not a property of the route or
 * of the connection, it is a property of whether a PTY exists, and the client
 * has to be told which or it either double-echoes or goes silent. These are the
 * only two legal states; ECHO_UNDECLARED is "we have not said anything yet". */
#define ECHO_UNDECLARED (-1)
#define ECHO_CLIENT     0  /* WONT ECHO: the client echoes for itself */
#define ECHO_PTY        1  /* WILL ECHO: the line discipline echoes */

struct mux_state {
  int            pty;        /* PTY master fd; -1 = none */
  int            pid;        /* child PID; -1 = none */
  int            auto_shell; /* spawn shell on first NAWS */
  int            owns_client;/* client connected to our GET:/tty route */
  int            echo_owner; /* ECHO_UNDECLARED / ECHO_CLIENT / ECHO_PTY */
  struct winsize wsz;
  struct termios tty;
  /* axil_generation() of the connection this state belongs to. This map is
   * keyed by fd, and the kernel recycles fd numbers, so an entry that outlived
   * its connection -- one whose teardown hook was missed, or that a module
   * forgot to release -- would otherwise look live to whatever connection took
   * the number next. mux_get() compares this and refuses a mismatch, which
   * keeps a stale pty and its child shell from being driven by, or fed to, an
   * unrelated request. SECURITY.md S5.4. */
  unsigned long long gen;
};

/* The newest NAWS seen on a connection, plus the generation it belongs to, so
 * a window size cannot be applied to a later connection that reused the fd. */
struct mux_wsz {
  unsigned long long gen;
  struct winsize ws;
};

/* State who owns echoing on this socket, and only actually send an option when
 * the owner changes. ECHO_UNDECLARED is what makes the first statement go out
 * even when the owner is already ECHO_CLIENT: a socket with no PTY has to be
 * told WONT ECHO just as much as one with a PTY has to be told WILL ECHO, or the
 * guest types into nothing.
 *
 * This replaces a one-way `echo_sent` latch that could only ever mean "have we
 * said WILL ECHO", which cannot express the two things that actually happen on a
 * socket another module owns: there is no PTY until a command asks for one (nd
 * runs its game and its shell on one /nd socket), and a PTY that exits hands the
 * socket back to a client that has to start echoing again. */
static void
mux_state_echo(socket_t cfd, struct mux_state *s, int owner)
{
  if (s->echo_owner == owner)
    return;
  TELNET_CMD(cfd, IAC, owner == ECHO_PTY ? WILL : WONT, TELOPT_ECHO);
  s->echo_owner = owner;
}

/* fd (uint32) → struct mux_state */
static uint32_t mux_map;
/* pty_fd (uint32) → client_fd (uint32) reverse lookup */
static uint32_t mux_pty_map;
/* fd (uint32) → struct mux_wsz, the latest NAWS seen on that connection.
 * Deliberately separate from mux_map: a client negotiates its window size as
 * soon as its socket opens, which is long before any command (and therefore
 * any PTY) exists. On a route we do not own there is no mux_state at all, so
 * the geometry has to live somewhere that does not require one. */
static uint32_t mux_wsz_map;

static uint32_t mux_state_type;  /* corm type id for struct mux_state */
static uint32_t mux_wsz_type;    /* corm type id for struct mux_wsz */

static void mux_init(void);

/* Every accessor below re-checks the generation. The map key is the fd, which
 * is not an identity: a new connection can land on the number of one that just
 * closed. Returning NULL for a mismatch is what makes that safe -- the caller
 * then takes its "I do not own this descriptor" path instead of driving
 * somebody else's pty. SECURITY.md S5.4. */
static struct mux_state *
mux_get(socket_t fd)
{
  if (!mux_map)
    return NULL;
  struct mux_state *s = (struct mux_state *)corm_get(mux_map, &(uint32_t){(uint32_t)fd});
  if (s && s->gen != axil_generation(fd))
    return NULL;
  return s;
}

static struct mux_wsz *
mux_wsz_get(socket_t fd)
{
  if (!mux_wsz_map)
    return NULL;
  struct mux_wsz *w = (struct mux_wsz *)corm_get(mux_wsz_map, &(uint32_t){(uint32_t)fd});
  if (w && w->gen != axil_generation(fd))
    return NULL;
  return w;
}

static void
mux_wsz_put(socket_t fd, const struct winsize *wsz)
{
  if (!mux_wsz_map)
    mux_init();
  struct mux_wsz w = { .gen = axil_generation(fd), .ws = *wsz };
  corm_put(mux_wsz_map, &(uint32_t){(uint32_t)fd}, &w);
}

static void
mux_wsz_del(socket_t fd)
{
  if (!mux_wsz_map)
    return;
  corm_del(mux_wsz_map, &(uint32_t){(uint32_t)fd});
}

static struct mux_state *
mux_put(socket_t fd, struct mux_state *s)
{
  if (!mux_map)
    mux_init();
  /* Stamp the identity of the connection this state is being created for, so
   * mux_get() can later tell it apart from a recycled fd. */
  s->gen = axil_generation(fd);
  corm_put(mux_map, &(uint32_t){(uint32_t)fd}, s);
  return mux_get(fd);
}

static void
mux_del(socket_t fd)
{
  if (!mux_map)
    return;
  corm_del(mux_map, &(uint32_t){(uint32_t)fd});
}

/* Helpers */

/* server-user password entry */
static struct passwd mux_pw;

static void
axil_tty_pw_copy(struct passwd *target, struct passwd *origin)
{
  *target = *origin;
  target->pw_name  = strdup(origin->pw_name);
  target->pw_shell = strdup(origin->pw_shell);
  target->pw_dir   = strdup(origin->pw_dir);
  target->pw_passwd = NULL;
}

static void
mux_init(void)
{
  if (mux_map)
    return;
  mux_state_type = corm_reg(sizeof(struct mux_state));
  mux_wsz_type   = corm_reg(sizeof(struct mux_wsz));
  mux_map     = corm_open(NULL, NULL, CM_U32, mux_state_type, 0xFF, 0);
  mux_pty_map = corm_open(NULL, NULL, CM_U32, CM_U32,         0xFF, 0);
  mux_wsz_map = corm_open(NULL, NULL, CM_U32, mux_wsz_type,   0xFF, 0);

  if (!mux_pw.pw_name) {
    char euname[BUFSIZ] = "root";
    struct passwd *pw = getpwuid(geteuid());
    if (pw)
      strncpy(euname, pw->pw_name, sizeof(euname) - 1);
    axil_tty_pw_copy(&mux_pw, getpwnam(euname));
  }
}

/* The line-discipline policy every PTY we create starts from, and the single
 * place that states it: the three creation sites used to each repeat a
 * different subset of these flags, and mux_ensure() never touched c_oflag at
 * all, so a guest reached that way got bare LFs.
 *
 * ECHO is deliberately left at the OS default (on), because the line discipline
 * is this session's only echo owner. That is what a real terminal is: the
 * driver echoes a byte as it arrives, which is why typing feels instant, while
 * the line itself stays buffered in the kernel until Enter. The client is
 * therefore told IAC WILL ECHO -- "I will echo, you will not" -- and stays a
 * pipe. It also has to stay that way for the whole session, so nothing here
 * clears ECHO and no code renegotiates it later: readline only echoes at all
 * when it inherits ECHO on, and a program that clears ECHO (vim) is about to
 * render the line itself. Two echoers is the one outcome nobody can recover
 * from, so there is never a second one.
 *
 * ECHOCTL is cleared, and that is the one c_lflag flag we set. Browsers send
 * DEL (0x7F) for Backspace and 0x7F is the Linux default erase character, so
 * n_tty.c:eraser() takes its iscntrl() branch and echoes BS DEL BS -- a literal
 * DEL on the wire, which is not something a terminal can render and which the
 * browser's parser reports as a parse error. With ECHOCTL off the same keypress
 * echoes BS SP BS, the sequence every terminal expects. */
static void
mux_pty_termios(socket_t pty, struct termios *t)
{
  tcgetattr(pty, t);
  t->c_iflag |= ICRNL;
  t->c_iflag &= ~(IGNCR | INLCR);
  t->c_oflag |= OPOST | ONLCR;
  t->c_oflag &= ~OCRNL;
  t->c_lflag &= ~ECHOCTL;
}

static struct mux_state *
mux_ensure(socket_t fd)
{
  mux_init();
  struct mux_state *s = mux_get(fd);
  if (s && s->pty >= 0)
    return s;

  struct mux_state new_s;
  if (s) {
    memcpy(&new_s, s, sizeof(new_s));
  } else {
    memset(&new_s, 0, sizeof(new_s));
    new_s.pty = -1;
    new_s.pid = -1;
    /* Undeclared, not the memset's 0: nothing has been said to this client yet,
     * and a state that claims ECHO_CLIENT would suppress a later
     * mux_state_echo(..., ECHO_CLIENT) as a no-change. */
    new_s.echo_owner = ECHO_UNDECLARED;
  }

  if (new_s.pty < 0) {
    new_s.pty = posix_openpt(O_RDWR | O_NOCTTY);
    if (new_s.pty == -1)
      return NULL;
    if (grantpt(new_s.pty) != 0 || unlockpt(new_s.pty) != 0) {
      close(new_s.pty);
      return NULL;
    }
    mux_pty_termios(new_s.pty, &new_s.tty);
    tcsetattr(new_s.pty, TCSANOW, &new_s.tty);
  }

  return mux_put(fd, &new_s);
}

static struct passwd *
drop_priviledges(socket_t fd)
{
  int euid = geteuid();

  struct passwd local_pw;
  struct passwd *pw;

  if (axil_get_pw(fd, &local_pw) == 0 && local_pw.pw_name && *local_pw.pw_name) {
    /* authenticated — use the connection user; pw_name etc. point into
       local_pw which is on the stack, but we only use it before execve */
    pw = &local_pw;
  } else {
    pw = &mux_pw;
  }

  if (!axil_config.chroot) {
    WARN("NOT_CHROOTED - running with %s\n", pw->pw_name);
    return pw;
  }

  if (euid != 0) {
    WARN("NOT_ROOT - skipping privilege drop for %s\n", pw->pw_name);
    return pw;
  }

  CBUG(!pw, "getpwnam\n");
  CBUG(setgroups(0, NULL), "setgroups\n");
  CBUG(initgroups(pw->pw_name, pw->pw_gid), "initgroups\n");
  CBUG(setgid(pw->pw_gid), "setgid\n");
  CBUG(setuid(pw->pw_uid), "setuid\n");

  return pw;
}

static int
mux_ensure_pty(socket_t cfd, struct mux_state *s)
{
  if (s->pty > 0) {
    axil_fd_unwatch(s->pty);
    if (mux_pty_map)
      corm_del(mux_pty_map, &(uint32_t){(uint32_t)s->pty});
    close(s->pty);
    s->pty = -1;
  }

  s->pty = posix_openpt(O_RDWR | O_NOCTTY);
  if (s->pty == -1)
    return -1;
  if (grantpt(s->pty) || unlockpt(s->pty)) {
    close(s->pty);
    s->pty = -1;
    return -1;
  }

  fcntl(s->pty, F_SETFL, O_NONBLOCK);

  mux_pty_termios(s->pty, &s->tty);
  tcsetattr(s->pty, TCSANOW, &s->tty);

  if (!mux_pty_map)
    mux_init();
  corm_put(mux_pty_map, &(uint32_t){(uint32_t)s->pty},
      &(uint32_t){(uint32_t)cfd});
  axil_fd_watch(s->pty);
  return 0;
}

/* PTY fork */

static inline int
command_pty(socket_t cfd, struct winsize *ws, char * const args[])
{
  struct mux_state *s = mux_get(cfd);
  CBUG(!s, "command_pty: no mux state for fd %d\n", cfd);
  WARN("command_pty: called for cfd=%d args[0]=%s\n", cfd, args[0] ? args[0] : "(null)");

  if (mux_ensure_pty(cfd, s) < 0)
    return -1;

  /* State the echo policy here, where the echoer is actually born: a PTY has
   * just been created and its line discipline is about to be the sole owner of
   * echoing. It used to be stated from on_axil_connect instead, which only runs
   * for our own GET:/tty route -- so a socket owned by another module (nd puts
   * its game and its shell on one /nd socket) got a PTY with nobody having said
   * WILL ECHO, and the client had been told nothing or worse. ECHO is a property
   * of the PTY, not of the route that happens to own the socket, so it belongs
   * on this path. Guarded on the owner rather than on "have we said it", because
   * a second command on a socket whose PTY died and was replaced has to say it
   * again, and one that replaced a live PTY must not. */
  mux_state_echo(cfd, s, ECHO_PTY);

  pid_t p = fork();
  if (p == 0) { /* child */
    axil_fork_child_reset();

    (void)setsid();

    int slave_fd = open(ptsname(s->pty), O_RDWR);
    if (slave_fd == -1)
      _exit(1);

    drop_priviledges(cfd);
    struct passwd local_pw;
    if (axil_get_pw(cfd, &local_pw) != 0 || !local_pw.pw_shell || !*local_pw.pw_shell) {
      local_pw = mux_pw;
    }

    close(s->pty);

    (void)ioctl(slave_fd, TIOCSCTTY, 0);
    if (ws && (ws->ws_row > 0 || ws->ws_col > 0))
      (void)ioctl(slave_fd, TIOCSWINSZ, ws);

    dup2(slave_fd, STDIN_FILENO);
    dup2(slave_fd, STDOUT_FILENO);
    dup2(slave_fd, STDERR_FILENO);
    if (slave_fd > 2)
      close(slave_fd);

    const char *sh = (local_pw.pw_shell && *local_pw.pw_shell) ? local_pw.pw_shell : "/bin/sh";
    char *alt_args[] = { (char *)sh, NULL };
    char * const *real_args = (args && args[0]) ? args : alt_args;
    char home[BUFSIZ], user[BUFSIZ], shell[BUFSIZ];
    snprintf(home,  sizeof(home),  "HOME=%s",  local_pw.pw_dir ? local_pw.pw_dir : "/tmp");
    snprintf(user,  sizeof(user),  "USER=%s",  local_pw.pw_name ? local_pw.pw_name : "user");
    snprintf(shell, sizeof(shell), "SHELL=%s", sh);

    char * const env[] = {
#ifdef __APPLE__
      /* /opt/homebrew is the arm64 Homebrew prefix; /usr/local is Intel's.
       * Entries that do not exist are ignored, so this stays correct on a
       * non-Homebrew macOS. dyld reads DYLD_LIBRARY_PATH, and SIP strips it
       * for SIP-protected binaries (/bin/sh, /usr/bin/man), so it is only
       * best-effort and is inherited by the shell's non-protected children. */
      "PATH=/opt/homebrew/bin:/opt/homebrew/sbin:/usr/bin:/bin:/usr/sbin:"
        "/sbin:/usr/local/bin",
      "DYLD_LIBRARY_PATH=/usr/local/lib:/opt/homebrew/lib",
#else
      "PATH=/bin:/usr/bin:/usr/local/bin",
      "LD_LIBRARY_PATH=/lib:/usr/lib:/usr/local/lib",
#endif
      "TERM=xterm-256color",
      "COLORTERM=truecolor",
      home, user, shell,
      NULL,
    };

    /* execvpe(3) is a glibc extension (POSIX.1-2008 TC1, but never implemented
     * on Darwin) and its implicit declaration is a hard error on clang >= 15,
     * which is what broke the arm64 brew build. Both of these are plain POSIX.
     * argv[0] is expected to be a full path, as it is everywhere else in axil
     * (see axil_posix popen2), so execve() carries our curated environment;
     * execvp() stays as the fallback for a bare name. */
    execve(real_args[0], real_args, env);
    execvp(real_args[0], real_args);
    _exit(127);
  }

  return p;
}

XY_IMPL(int, axil_tty_exec,
    socket_t, fd,
    char **, argv)
{
  struct mux_state *s = mux_ensure(fd);
  if (!s)
    return -1;
  /* The map holds the newest NAWS, which is later than anything in s->wsz when
   * the state was created by mux_ensure() rather than by on_axil_connect(). */
  struct mux_wsz *w = mux_wsz_get(fd);
  if (w)
    s->wsz = w->ws;
  s->pid = command_pty(fd, &s->wsz, (char * const *)argv);
  axil_fd_watch(s->pty);
  return 0;
}

XY_IMPL(int, axil_tty_shell, socket_t, fd)
{
  /* NULL argv selects command_pty()'s alt_args: the connection user's login
   * shell, falling back through mux_pw to /bin/sh (see b0485b0's first
   * hunk). Passing an explicit argv[0] here would bypass that whole chain
   * -- which is how every `sh` silently landed in dash, a canonical-mode
   * reader with no line editing, where arrow keys arrive as raw escape
   * bytes instead of history. */
  char *argv[] = { NULL, NULL };
  return axil_tty_exec(fd, argv);
}

XY_IMPL(int, axil_tty_active, socket_t, fd)
{
  struct mux_state *s = mux_get(fd);
  return (s && s->pid > 0) ? 1 : 0;
}

XY_IMPL(int, axil_tty_owns, socket_t, fd)
{
  struct mux_state *s = mux_get(fd);
  return (s && s->owns_client) ? 1 : 0;
}

XY_IMPL(int, axil_tty_attach, socket_t, fd)
{
  /* Create the per-connection state without opening a PTY. mux_ensure() cannot
   * be reused here: it opens a PTY as a side effect, and the point of attach is
   * to negotiate a socket whose PTY does not exist yet -- the shell arrives
   * later, from axil_tty_shell()/axil_tty_exec(), and command_pty() is where
   * ECHO is stated for it. */
  mux_init();
  struct mux_state *s = mux_get(fd);
  if (!s) {
    struct mux_state new_s;
    memset(&new_s, 0, sizeof(new_s));
    new_s.pty = -1;
    new_s.pid = -1;
    /* Not left at the memset's 0, which is ECHO_CLIENT: it has to read as
     * "nothing said yet" or the statement below is suppressed as a no-change and
     * the client is told nothing at all. */
    new_s.echo_owner = ECHO_UNDECLARED;
    /* auto_shell and owns_client stay 0: spawning a shell on first NAWS and
     * claiming the /tty route are both properties of our own route handler,
     * not of a socket somebody else owns. */
    s = mux_put(fd, &new_s);
    if (!s)
      return -1;
  }

  /* WONT ECHO, not WILL. There is no PTY yet, and nothing else on this socket is
   * going to echo either: the module that owns it runs a game there until a
   * command asks for a shell, and a game does not echo its input line. Saying
   * WILL here is the one thing that cannot be true of this moment, and it used to
   * be the only thing said -- so the client sat as a pipe told the server would
   * echo, with no echoer, and every keystroke vanished until a PTY happened to
   * come along. Stating WONT makes the client the echo owner, which is what is
   * actually true, and command_pty() hands the socket over when the driver
   * becomes the owner in its turn. Guarded on the owner, so a second attach is
   * not a new policy but a returning socket still is. */
  mux_state_echo(fd, s, ECHO_CLIENT);
  /* Re-stated unconditionally: the client is a browser terminal that will not
   * send a window size unless asked, and this is a fresh connection. */
  TELNET_CMD(fd, IAC, WONT, TELOPT_SGA);
  TELNET_CMD(fd, IAC, DO, TELOPT_NAWS);
  return 0;
}

static void
do_sh(socket_t fd,
    int argc UNUSED,
    char *argv[] UNUSED)
{
  axil_tty_shell(fd);
}

/* axil hook implementations */

XY_IMPL(int, on_axil_connect, socket_t, fd)
{
  /* axil dispatches this hook to every loaded module for every WebSocket
   * upgrade, whatever route matched. Only claim the one we registered. */
  char doc_uri[BUFSIZ] = "";
  axil_env_get(fd, doc_uri, sizeof(doc_uri), "DOCUMENT_URI");
  if (strcmp(doc_uri, AXIL_TTY_ROUTE) != 0)
    return 0;

  struct mux_state s;
  memset(&s, 0, sizeof(s));
  s.pty = -1;
  s.pid = -1;
  s.echo_owner = ECHO_UNDECLARED;

  /* Send initial TELNET negotiations. WILL ECHO, and only ever once: the PTY
   * below keeps the line discipline's own ECHO, so this is the truth, and it is
   * the contract the client codes against -- it stays a pipe and lets the
   * driver echo each keystroke as it arrives. command_pty() is where that
   * handover normally happens; on this route the PTY is born right here, so
   * state it here and record that it has been said. This route never calls
   * axil_tty_attach(), so this is a first statement, not a change of owner --
   * hence the direct TELNET_CMD rather than mux_state_echo(), which would need
   * the state to be in the map first. */
  TELNET_CMD(fd, IAC, WILL, TELOPT_ECHO);
  TELNET_CMD(fd, IAC, WONT, TELOPT_SGA);
  TELNET_CMD(fd, IAC, DO, TELOPT_NAWS);
  s.echo_owner = ECHO_PTY;

  CBUG(fcntl(fd, F_SETFL, O_NONBLOCK) == -1,
      "telnet_connected fcntl F_SETFL O_NONBLOCK\n");

  s.pty = posix_openpt(O_RDWR | O_NOCTTY);
  CBUG(s.pty == -1,  "telnet_connected posix_openpt\n");
  CBUG(grantpt(s.pty),  "telnet_connected grantpt\n");
  CBUG(unlockpt(s.pty), "telnet_connected unlockpt\n");

  /* Start from the OS defaults, then apply our one policy: the line discipline
   * keeps its ECHO, because it is the only echoer on this connection. */
  mux_pty_termios(s.pty, &s.tty);

  struct mux_state *sp = mux_put(fd, &s);

  /* We are the reader for this connection, but not by claiming it: axil decodes
   * the client's frames and delivers each payload to on_axil_parse, which is
   * axil_tty_input(). Only the PTY master below is watched. */
  sp->owns_client = 1;

  /* reverse pty→client map */
  corm_put(mux_pty_map, &(uint32_t){(uint32_t)sp->pty},
      &(uint32_t){(uint32_t)fd});

  tcsetattr(sp->pty, TCSANOW, &sp->tty);

  if (sp->wsz.ws_col || sp->wsz.ws_row)
    ioctl(sp->pty, TIOCSWINSZ, &sp->wsz);

  /* Auto-spawn a shell on first NAWS. Only our own route gets here, so a host
   * module that borrows this PTY (axil-nd's `sh`/`man`) never has a shell
   * appear under it. */
  sp->auto_shell = 1;

  return 0;
}

XY_IMPL(int, axil_tty_input,
    socket_t, fd,
    unsigned char *, input,
    int, nread)
{
  /* Absent state is normal on a route we do not own: the telnet options are
   * still ours to consume (the geometry is recorded in mux_wsz_map for a PTY
   * that may not exist yet), but there is no PTY to write the remainder to. */
  struct mux_state *s = mux_get(fd);

  int i = 0;

  for (; i < nread && input[i] != IAC; i++);

  if (i == nread)
    i = 0;

  while (i < nread && input[i + 0] == IAC) {
    if (i + 2 >= nread)
      break;                       /* truncated option: leave the rest alone */

    if (input[i + 1] == SB && input[i + 2] == TELOPT_NAWS) {
      if (i + 6 >= nread)
        break;                     /* truncated subnegotiation payload */

      struct winsize wsz;
      memset(&wsz, 0, sizeof(wsz));
      wsz.ws_col = (input[i + 3] << 8) | input[i + 4];
      wsz.ws_row = (input[i + 5] << 8) | input[i + 6];

      mux_wsz_put(fd, &wsz);

      if (s) {
        s->wsz = wsz;
        if (s->pty > 0)
          ioctl(s->pty, TIOCSWINSZ, &s->wsz);
      }

      i += 9;

      /* First NAWS received — spawn shell now that dimensions are set */
      if (s && s->auto_shell && s->pid == -1) {
        s->auto_shell = 0;
        axil_tty_shell(fd);
      }
    } else if (input[i + 1] == DO && input[i + 2] == TELOPT_SGA) {
      i += 3;
    } else if (input[i + 1] == DO) {
      i += 3;
    } else if (input[i + 1] == DONT) {
      i += 3;
    } else if (input[i + 1] == WILL) {
      i += 3;
    } else {
      i++;
    }
  }

  if (s && s->pid > 0 && i < nread) {
    /* Hand the rest to the pty only for a connection this module still owns.
     * mux_get() already rejected a recycled fd, so reaching here means the
     * generation matched; the echo of the socket number and the first bytes is
     * diagnostic for a case that should be impossible, and is opt-in so a
     * silent corruption cannot hide in a normal log. */
    if (getenv("AXIL_TTY_TRACE"))
      fprintf(stderr, "axil_tty: fd=%d gen=%llu pty=%d pid=%d feeding %d bytes\n",
              fd, s->gen, s->pty, s->pid, nread - i);
    write(s->pty, input + i, nread - i);
    return -1; /* signal: consumed by PTY, skip cmd_parse */
  }

  return i;
}

XY_IMPL(int, on_axil_parse,
    socket_t, fd,
    unsigned char *, input,
    int, nread)
{
  /* HTTP request bytes are never telnet: the head routes in cmd_parse and a
   * body may legally contain 0xFF, which the IAC scan below would misread as
   * negotiation (and, through the caller's slide, delete the head in front of
   * it -- SECURITY.md S5.5). WebSocket payloads are exempt: shell bytes ride
   * frames and must still reach the PTY. Method list mirrors the one in
   * axil-nd's is_http_method; both answer "does this chunk open with a request
   * line" and nothing more. */
  if (!(axil_flags(fd) & DF_WEBSOCKET) && nread > 5) {
    static const char *const methods[] = {
      "GET ", "POST ", "HEAD ", "PUT ", "OPTIONS ", "DELETE ", "PATCH ", "PRI "
    };
    size_t i = 0;
    while (input[i] == ' ')
      i++;
    for (size_t m = 0; m < sizeof(methods) / sizeof(methods[0]); m++) {
      size_t n = strlen(methods[m]);
      if (i + n < (size_t)nread && strncmp((const char *)input + i, methods[m], n) == 0)
        return 0;
    }
  }
  return axil_tty_input(fd, input, nread);
}

XY_IMPL(int, on_axil_tick, socket_t, fd) {
  if (!mux_pty_map)
    return 0;

  /* Only a PTY master is ever watched now. The client side is unclaimed, so axil
   * decodes its frames and delivers the payloads to on_axil_parse, and a
   * borrowed socket is handled the same way -- which is what lets a host module
   * such as axil-nd drop its own frame reading entirely. The only descriptors
   * left reaching here are the PTY masters axil_fd_watch() was called on. */

  /* fd here is a watched PTY master — look up the client fd */
  const uint32_t *cfd_p = corm_get(mux_pty_map, &(uint32_t){(uint32_t)fd});

  if (!cfd_p) {
    axil_clear_active(fd);
    return -1;
  }

  socket_t cfd = (socket_t)*cfd_p;

  struct mux_state *s = mux_get(cfd);
  if (!s) {
    axil_clear_active(fd);
    return -1;
  }

  static char buf[BUFSIZ * 4];
  int ret, status;

  memset(buf, 0, sizeof(buf));
  errno = 0;
  ret = read(fd, buf, sizeof(buf));

  switch (ret) {
    case 0:
      if (s->pid > 0 && waitpid(s->pid, &status, WNOHANG) == 0)
        return 0;
      break;
    case -1:
      if (errno == EAGAIN)
        return 0;
      if (errno == EIO) {
        if (s->pid > 0 && waitpid(s->pid, &status, WNOHANG) == 0)
          return 0;
        break;
      }
      axil_clear_active(fd);
      return -1;
  default:
    axil_write(cfd, buf, ret);
    goto exit;
  }

  if (s->pid > 0)
    kill(s->pid, SIGKILL);

  s->pid = -1;
  if (s->pty > 0) {
    axil_fd_unwatch(s->pty);
    if (mux_pty_map)
      corm_del(mux_pty_map, &(uint32_t){(uint32_t)s->pty});
    close(s->pty);
    s->pty = -1;
  }
  /* The child is gone and the PTY above is closed, so this socket has no echoer
   * of its own again and the client has to take the job back -- the mirror image
   * of axil_tty_attach() and of command_pty()'s handover, and the reason a guest
   * who leaves a shell session is not left typing into a dead pipe. This used to
   * re-assert WILL ECHO on the grounds that the policy was fixed per socket, but
   * the policy is not fixed: it tracks whether a PTY exists, and one just stopped
   * existing. */
  mux_state_echo(cfd, s, ECHO_CLIENT);
  TELNET_CMD(cfd, IAC, WONT, TELOPT_SGA);
exit:
  if (ret < 0)
    axil_clear_active(fd);
  return ret;
}

XY_IMPL(int, on_axil_disconnect, socket_t, fd) {
  if (!mux_map)
    return 0;
  /* Before the mux_state lookup: fd numbers are reused, so a geometry entry
   * left behind here would be applied to whatever connection lands on this fd
   * next — and on a route we do not own there is no mux_state to find. */
  mux_wsz_del(fd);
  struct mux_state *s = mux_get(fd);
  if (!s)
    return 0;

  if (s->pty > 0) {
    if (s->pid > 0)
      kill(s->pid, SIGKILL);
    s->pid = -1;
    axil_fd_unwatch(s->pty);
    if (mux_pty_map)
      corm_del(mux_pty_map, &(uint32_t){(uint32_t)s->pty});
    close(s->pty);
    s->pty = -1;
  }

  mux_del(fd);
  return 0;
}

/* Asset serving */

static void
serve_htdocs(socket_t fd, const char *file)
{
  char htdocs[PATH_MAX - 1] = AXIL_HTDOCS;
  char path[PATH_MAX];
  /* Server-side override, read from the process environment. It is
   * deliberately not taken from the per-connection request env, which is
   * populated from client input. */
  const char *override = getenv("AXIL_HTDOCS");
  if (override && *override)
    snprintf(htdocs, sizeof(htdocs), "%s", override);
  snprintf(path, sizeof(path), "%s/%s", htdocs, file);
  axil_sendfile(fd, path);
}

static int
handle_axil_tty_js(socket_t fd, char *body)
{
  (void)body;
  serve_htdocs(fd, "axil-tty.js");
  return 0;
}

static int
handle_axil_tty_css(socket_t fd, char *body)
{
  (void)body;
  serve_htdocs(fd, "axil-tty.css");
  return 0;
}

static int
handle_demo_js(socket_t fd, char *body)
{
  (void)body;
  serve_htdocs(fd, "demo.js");
  return 0;
}

int
axil_tty_handle_tty(socket_t fd, char *body)
{
  (void)body;
  char key[ENV_VALUE_LEN] = {0};
  if (axil_env_get(fd, key, sizeof(key), "HTTP_SEC_WEBSOCKET_KEY") == 0) {
    axil_ws_upgrade(fd);
    /* No axil_fd_watch() here on purpose. This used to be mandatory, with a
     * comment saying so: axil did not route WebSocket frames through
     * on_axil_parse, so a module had to claim the socket and read them from
     * on_axil_tick. axil now decodes the frame itself and hands the payload to
     * on_axil_parse, so the client needs no claim at all -- and claiming it
     * would be actively wrong. axil_fd_watch() sets DF_EXTERN, which takes the
     * descriptor out of descr_read() and gives it to axil_fd_tick() instead,
     * so this module's own on_axil_tick would get the frames and
     * on_axil_parse would never see a byte. */
    return 0;
  }
  serve_htdocs(fd, "index.html");
  return 0;
}

/* Module entry points */

void
xy_install(void)
{
  /* Allocate corm types and maps */
  mux_init();

  /* Register the shell command */
  axil_register("sh", do_sh, CF_NOTRIM);

  /* Serve browser terminal assets */
  axil_register_handler("GET:/axil-tty.js",  handle_axil_tty_js);
  axil_register_handler("GET:/axil-tty.css", handle_axil_tty_css);
  axil_register_handler("GET:/demo.js",      handle_demo_js);
  axil_register_handler("GET:" AXIL_TTY_ROUTE, axil_tty_handle_tty);
}
