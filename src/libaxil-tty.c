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
  struct winsize wsz;
  struct termios tty;
};

/* fd (uint32) → struct mux_state */
static uint32_t mux_map;
/* pty_fd (uint32) → client_fd (uint32) reverse lookup */
static uint32_t mux_pty_map;

static uint32_t mux_state_type;  /* corm type id for struct mux_state */

static struct mux_state *
mux_get(socket_t fd)
{
  return (struct mux_state *)corm_get(mux_map, &(uint32_t){(uint32_t)fd});
}

static struct mux_state *
mux_put(socket_t fd, struct mux_state *s)
{
  corm_put(mux_map, &(uint32_t){(uint32_t)fd}, s);
  return (struct mux_state *)corm_get(mux_map, &(uint32_t){(uint32_t)fd});
}

static void
mux_del(socket_t fd)
{
  corm_del(mux_map, &(uint32_t){(uint32_t)fd});
}

/* Helpers */

/* server-user password entry */
static struct passwd mux_pw;

static void
axil_tty_pw_free(struct passwd *target)
{
  free(target->pw_name);
  free(target->pw_shell);
  free(target->pw_dir);
}

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

  if (axil_get_pw(fd, &local_pw) == 0) {
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

/* PTY fork */

static inline int
command_pty(socket_t cfd, struct winsize *ws, char * const args[])
{
  struct mux_state *s = mux_get(cfd);
  CBUG(!s, "command_pty: no mux state for fd %d\n", cfd);

  axil_fd_watch(s->pty);

  /* Mark the pty fd's reverse entry as "this is a pty master" (pid=-2) */
  struct mux_state pty_sentinel = { .pty = -2, .pid = -2 };
  corm_put(mux_pty_map, &(uint32_t){(uint32_t)s->pty},
      &(uint32_t){(uint32_t)cfd});
  (void)pty_sentinel; /* used above for documentation */

  pid_t p = fork();
  if (p == 0) { /* child */
    axil_fork_child_reset();

    CBUG(setsid() == -1, "setsid\n");

    CBUG(!(axil_flags(cfd) & DF_AUTHENTICATED), "NOT AUTHENTICATED\n");

    int slave_fd = open(ptsname(s->pty), O_RDWR);
    CBUG(slave_fd == -1, "open %d\n", errno);

    drop_priviledges(cfd);
    struct passwd local_pw;
    if (axil_get_pw(cfd, &local_pw) != 0) {
      local_pw = mux_pw;
    }

    (void) fcntl(slave_fd, F_GETFL, 0);

    close(s->pty);

    CBUG(ioctl(slave_fd, TIOCSCTTY, NULL) == -1, "ioctl TIOCSCTTY\n");
    CBUG(ioctl(slave_fd, TIOCSWINSZ, ws)  == -1, "ioctl TIOCSWINSZ\n");
    CBUG(fcntl(slave_fd, F_SETFD, FD_CLOEXEC) == -1,
        "fcntl srv_fd F_SETFL FD_CLOEXEC\n");

    CBUG(dup2(slave_fd, STDIN_FILENO)  == -1, "dup2 STDIN\n");
    CBUG(dup2(slave_fd, STDOUT_FILENO) == -1, "dup2 STDOUT\n");
    CBUG(dup2(slave_fd, STDERR_FILENO) == -1, "dup2 STDERR\n");

    char *alt_args[] = { local_pw.pw_shell, NULL };
    char * const *real_args = args[0] ? args : alt_args;
    char home[BUFSIZ], user[BUFSIZ], shell[BUFSIZ];
    snprintf(home,  sizeof(home),  "HOME=%s",  local_pw.pw_dir);
    snprintf(user,  sizeof(user),  "USER=%s",  local_pw.pw_name);
    snprintf(shell, sizeof(shell), "SHELL=%s", local_pw.pw_shell);

    char * const env[] = {
      "PATH=/bin:/usr/bin:/usr/local/bin",
      "LD_LIBRARY_PATH=/lib:/usr/lib:/usr/local/lib",
      "TERM=xterm-256color",
      "COLORTERM=truecolor",
      home, user, shell,
      NULL,
    };

    execve(real_args[0], real_args, env);
    CBUG(1, "execve\n");
  }

  return p;
}

XY_IMPL(int, axil_tty_exec,
    socket_t, fd,
    char **, argv)
{
  struct mux_state *s = mux_get(fd);
  if (!s)
    return -1;
  s->pid = command_pty(fd, &s->wsz, (char * const *)argv);
  axil_fd_watch(s->pty);
  return 0;
}

XY_IMPL(int, axil_tty_shell, socket_t, fd)
{
  char *argv[] = { NULL, NULL };
  return axil_tty_exec(fd, argv);
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

  /* reverse pty→client map */
  corm_put(mux_pty_map, &(uint32_t){(uint32_t)sp->pty},
      &(uint32_t){(uint32_t)fd});

  tcsetattr(sp->pty, TCSANOW, &sp->tty);
  axil_tty_update(fd);

  if (sp->wsz.ws_col || sp->wsz.ws_row)
    ioctl(sp->pty, TIOCSWINSZ, &sp->wsz);

  /* Auto-spawn a shell on first NAWS when connected via /tty */
  char doc_uri[BUFSIZ] = "";
  axil_env_get(fd, doc_uri, sizeof(doc_uri), "DOCUMENT_URI");
  if (strcmp(doc_uri, "/tty") == 0)
    sp->auto_shell = 1;

  return 0;
}

XY_IMPL(int, on_axil_parse,
    socket_t, fd,
    unsigned char *, input,
    int, nread)
{
  struct mux_state *s = mux_get(fd);
  if (!s)
    return 0;

  int i = 0;

  for (; i < nread && input[i] != IAC; i++);

  if (i == nread)
    i = 0;

  while (i < nread && input[i + 0] == IAC) {
    if (input[i + 1] == SB && input[i + 2] == TELOPT_NAWS) {
      unsigned char colsHighByte = input[i + 3];
      unsigned char colsLowByte  = input[i + 4];
      unsigned char rowsHighByte = input[i + 5];
      unsigned char rowsLowByte  = input[i + 6];

      memset(&s->wsz, 0, sizeof(s->wsz));
      s->wsz.ws_col = (colsHighByte << 8) | colsLowByte;
      s->wsz.ws_row = (rowsHighByte << 8) | rowsLowByte;

      if (s->pty > 0)
        ioctl(s->pty, TIOCSWINSZ, &s->wsz);

      i += 9;

      /* First NAWS received — spawn shell now that dimensions are set */
      if (s->auto_shell && s->pid == -1) {
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

  if (s->pid > 0 && i < nread) {
    write(s->pty, input + i, nread - i);
    return -1; /* signal: consumed by PTY, skip cmd_parse */
  }

  return i;
}

XY_IMPL(int, on_axil_tick, socket_t, fd) {
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
      if (errno == EAGAIN || errno == EIO)
        return 0;
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
exit:
  if (ret < 0)
    axil_clear_active(fd);
  return ret;
}

XY_IMPL(int, on_axil_disconnect, socket_t, fd) {
  struct mux_state *s = mux_get(fd);
  if (!s)
    return 0;

  if (s->pty > 0) {
    if (s->pid > 0)
      kill(-s->pid, SIGKILL);
    s->pid = -1;
    axil_fd_unwatch(s->pty);
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
  axil_env_get(fd, htdocs, sizeof(htdocs), "AXIL_HTDOCS");
  snprintf(path, sizeof(path), "%s/%s", htdocs, file);
  axil_sendfile(fd, path);
}

static int
handle_axil_js(socket_t fd, char *body)
{
  (void)body;
  serve_htdocs(fd, "axil.js");
  return 0;
}

static int
handle_axil_css(socket_t fd, char *body)
{
  (void)body;
  serve_htdocs(fd, "axil.css");
  return 0;
}

static int
handle_axil_tty_js(socket_t fd, char *body)
{
  (void)body;
  serve_htdocs(fd, "axil-tty.js");
  return 0;
}

static int
handle_tty(socket_t fd, char *body)
{
  (void)body;
  char key[ENV_VALUE_LEN] = {0};
  if (axil_env_get(fd, key, sizeof(key), "HTTP_SEC_WEBSOCKET_KEY") == 0) {
    axil_ws_upgrade(fd);
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
  mux_state_type = corm_reg(sizeof(struct mux_state));
  mux_map     = corm_open(NULL, NULL, CM_U32, mux_state_type, 0xFF, 0);
  mux_pty_map = corm_open(NULL, NULL, CM_U32, CM_U32,         0xFF, 0);

  /* Cache server-user pw entry */
  char euname[BUFSIZ] = "root";
  strncpy(euname, getpwuid(geteuid())->pw_name, sizeof(euname) - 1);
  axil_tty_pw_copy(&mux_pw, getpwnam(euname));

  /* Register the shell command */
  axil_register("sh", do_sh, CF_NOTRIM);

  /* Serve browser terminal assets */
  axil_register_handler("GET:/axil.js",     handle_axil_js);
  axil_register_handler("GET:/axil.css",    handle_axil_css);
  axil_register_handler("GET:/axil-tty.js", handle_axil_tty_js);
  axil_register_handler("GET:/tty",         handle_tty);
}