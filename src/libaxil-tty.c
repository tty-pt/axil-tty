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

struct mux_state {
  int            pty;        /* PTY master fd; -1 = none */
  int            pid;        /* child PID; -1 = none */
  int            auto_shell; /* spawn shell on first NAWS */
  int            owns_client;/* client connected to our GET:/tty route */
  struct winsize wsz;
  struct termios tty;
};

/* fd (uint32) → struct mux_state */
static uint32_t mux_map;
/* pty_fd (uint32) → client_fd (uint32) reverse lookup */
static uint32_t mux_pty_map;
/* fd (uint32) → struct winsize, the latest NAWS seen on that connection.
 * Deliberately separate from mux_map: a client negotiates its window size as
 * soon as its socket opens, which is long before any command (and therefore
 * any PTY) exists. On a route we do not own there is no mux_state at all, so
 * the geometry has to live somewhere that does not require one. */
static uint32_t mux_wsz_map;

static uint32_t mux_state_type;  /* corm type id for struct mux_state */
static uint32_t mux_wsz_type;    /* corm type id for struct winsize */

static void mux_init(void);

static struct mux_state *
mux_get(socket_t fd)
{
  if (!mux_map)
    return NULL;
  return (struct mux_state *)corm_get(mux_map, &(uint32_t){(uint32_t)fd});
}

static struct winsize *
mux_wsz_get(socket_t fd)
{
  if (!mux_wsz_map)
    return NULL;
  return (struct winsize *)corm_get(mux_wsz_map, &(uint32_t){(uint32_t)fd});
}

static void
mux_wsz_put(socket_t fd, const struct winsize *wsz)
{
  if (!mux_wsz_map)
    mux_init();
  corm_put(mux_wsz_map, &(uint32_t){(uint32_t)fd}, (void *)wsz);
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
  corm_put(mux_map, &(uint32_t){(uint32_t)fd}, s);
  return (struct mux_state *)corm_get(mux_map, &(uint32_t){(uint32_t)fd});
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
  mux_wsz_type   = corm_reg(sizeof(struct winsize));
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
  }

  if (new_s.pty < 0) {
    new_s.pty = posix_openpt(O_RDWR | O_NOCTTY);
    if (new_s.pty == -1)
      return NULL;
    if (grantpt(new_s.pty) != 0 || unlockpt(new_s.pty) != 0) {
      close(new_s.pty);
      return NULL;
    }
    tcgetattr(new_s.pty, &new_s.tty);
    new_s.tty.c_iflag |= ICRNL;
    new_s.tty.c_iflag &= ~(IGNCR | INLCR);
    tcsetattr(new_s.pty, TCSANOW, &new_s.tty);
  }

  return mux_put(fd, &new_s);
}

static void
axil_tty_update(socket_t fd)
{
  struct mux_state *s = mux_get(fd);
  if (!s || s->pty < 0)
    return;

  struct termios last = s->tty;
  tcgetattr(s->pty, &s->tty);

  if ((last.c_lflag & ECHO) != (s->tty.c_lflag & ECHO))
    TELNET_CMD(fd, IAC, s->tty.c_lflag & ECHO ? WILL : WONT, TELOPT_ECHO);

  if ((last.c_lflag & ICANON) != (s->tty.c_lflag & ICANON))
    TELNET_CMD(fd, IAC, s->tty.c_lflag & ICANON ? WONT : WILL, TELOPT_SGA);
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

  tcgetattr(s->pty, &s->tty);
  s->tty.c_iflag |= ICRNL;
  s->tty.c_iflag &= ~(IGNCR | INLCR);
  s->tty.c_oflag |= OPOST | ONLCR;
  s->tty.c_oflag &= ~OCRNL;
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
  const struct winsize *wsz = mux_wsz_get(fd);
  if (wsz)
    s->wsz = *wsz;
  s->pid = command_pty(fd, &s->wsz, (char * const *)argv);
  axil_fd_watch(s->pty);
  return 0;
}

XY_IMPL(int, axil_tty_shell, socket_t, fd)
{
  char *argv[] = { "/bin/sh", NULL };
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

  /* Send initial TELNET negotiations */
  TELNET_CMD(fd, IAC, WILL, TELOPT_ECHO);
  TELNET_CMD(fd, IAC, WONT, TELOPT_SGA);
  TELNET_CMD(fd, IAC, DO, TELOPT_NAWS);

  CBUG(fcntl(fd, F_SETFL, O_NONBLOCK) == -1,
      "telnet_connected fcntl F_SETFL O_NONBLOCK\n");

  s.pty = posix_openpt(O_RDWR | O_NOCTTY);
  CBUG(s.pty == -1,  "telnet_connected posix_openpt\n");
  CBUG(grantpt(s.pty),  "telnet_connected grantpt\n");
  CBUG(unlockpt(s.pty), "telnet_connected unlockpt\n");

  /* Start from OS defaults, then adjust only CR/LF translation */
  tcgetattr(s.pty, &s.tty);
  s.tty.c_iflag |= ICRNL;
  s.tty.c_iflag &= ~(IGNCR | INLCR);
  s.tty.c_oflag |= OPOST | ONLCR;
  s.tty.c_oflag &= ~OCRNL;

  struct mux_state *sp = mux_put(fd, &s);

  /* We are the reader for this connection: axil drops WebSocket frames for the
   * module to pull (it never sends them through on_axil_parse), so handle_tty
   * watches the client fd and this module's on_axil_tick drains it. */
  sp->owns_client = 1;

  /* reverse pty→client map */
  corm_put(mux_pty_map, &(uint32_t){(uint32_t)sp->pty},
      &(uint32_t){(uint32_t)fd});

  tcsetattr(sp->pty, TCSANOW, &sp->tty);
  axil_tty_update(fd);

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
  return axil_tty_input(fd, input, nread);
}

XY_IMPL(int, on_axil_tick, socket_t, fd) {
  if (!mux_pty_map)
    return 0;

  if (axil_flags(fd) & DF_WEBSOCKET) {
    /* Client socket. axil deliberately does not hand WebSocket frames to
     * on_axil_parse, so the module that owns the connection has to read them
     * here. A borrowed connection belongs to the host module (axil-nd watches
     * the fd itself and forwards through axil_tty_input), so leave it alone --
     * two ticks draining one socket would split the frame stream. */
    struct mux_state *s = mux_get(fd);
    if (!s || !s->owns_client)
      return 0;

    static char cbuf[BUFSIZ * 4];
    ssize_t n;
    while ((n = axil_ws_read(fd, cbuf, sizeof(cbuf))) > 0)
      axil_tty_input(fd, (unsigned char *)cbuf, (int)n);

    return 0;
  }

  /* FD_EXTERN is shared with modules that watch client fds (axil-nd calls
   * axil_fd_watch on the WebSocket client so it can read client frames). We
   * only ever watch PTY masters ourselves, so a WebSocket fd here is someone
   * else's descriptor. Falling through would hit the mux_pty_map miss below,
   * which calls axil_clear_active(fd) == FD_CLR(fd, &fds_active) and would
   * drop a live client from the select set. */
  /* fd here is an externally-watched fd — look up the client fd */
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
      buf[ret] = '\0';
      axil_write(cfd, buf, ret);
      axil_tty_update(cfd);
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
  TELNET_CMD(cfd, IAC, WILL, TELOPT_ECHO);
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

static int
handle_tty(socket_t fd, char *body)
{
  (void)body;
  char key[ENV_VALUE_LEN] = {0};
  if (axil_env_get(fd, key, sizeof(key), "HTTP_SEC_WEBSOCKET_KEY") == 0) {
    axil_ws_upgrade(fd);
    /* Mandatory after the handshake: axil does not route WebSocket frames
     * through on_axil_parse, so without this nothing ever reads the client and
     * axil_tty_input() is never called. It is what turns the descriptor into
     * axil_fd_tick()'s, which reaches this module's on_axil_tick. */
    axil_fd_watch(fd);
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
  axil_register_handler("GET:" AXIL_TTY_ROUTE, handle_tty);
}
