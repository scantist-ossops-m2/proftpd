/*
 * ProFTPD - FTP server daemon
 * Copyright (c) 1997, 1998 Public Flood Software
 * Copyright (c) 1999, 2000 MacGyver aka Habeeb J. Dihu <macgyver@tos.net>
 * Copyright (c) 2001-2011 The ProFTPD Project team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA.
 *
 * As a special exemption, Public Flood Software/MacGyver aka Habeeb J. Dihu
 * and other respective copyright holders give permission to link this program
 * with OpenSSL, and distribute the resulting executable, without including
 * the source code for OpenSSL in the source distribution.
 */

/* ProFTPD logging support.
 * $Id: log.c,v 1.110 2011-12-12 04:23:33 castaglia Exp $
 */

#include "conf.h"

/* Max path length plus 64 bytes for additional info. */
#define LOGBUFFER_SIZE		(PR_TUNABLE_PATH_MAX + 64)

static int syslog_open = FALSE;
static int syslog_discard = FALSE;
static int logstderr = TRUE;
static int debug_level = DEBUG0;	/* Default is no debug logging */
static int facility = LOG_DAEMON;
static int set_facility = -1;
static char systemlog_fn[PR_TUNABLE_PATH_MAX] = {'\0'};
static char systemlog_host[256] = {'\0'};
static int systemlog_fd = -1;

int syslog_sockfd = -1;

#ifdef PR_USE_NONBLOCKING_LOG_OPEN
static int fd_set_block(int fd) {
  int flags, res;

  flags = fcntl(fd, F_GETFL);
  res = fcntl(fd, F_SETFL, flags & (U32BITS ^ O_NONBLOCK));

  return res;
}
#endif /* PR_USE_NONBLOCKING_LOG_OPEN */

int pr_log_openfile(const char *log_file, int *log_fd, mode_t log_mode) {
  int res;
  pool *tmp_pool = NULL;
  char *tmp = NULL, *lf;
  unsigned char have_stat = FALSE, *allow_log_symlinks = NULL;
  struct stat st;

  /* Sanity check */
  if (!log_file || !log_fd) {
    errno = EINVAL;
    return -1;
  }

  /* Make a temporary copy of log_file in case it's a constant */
  tmp_pool = make_sub_pool(permanent_pool);
  pr_pool_tag(tmp_pool, "log_openfile() tmp pool");
  lf = pstrdup(tmp_pool, log_file);

  tmp = strrchr(lf, '/');
  if (tmp == NULL) {
    pr_log_debug(DEBUG0, "inappropriate log file: %s", lf);
    destroy_pool(tmp_pool);

    errno = EINVAL;
    return -1;
  }

  /* Set the path separator to zero, in order to obtain the directory
   * name, so that checks of the directory may be made.
   */
  *tmp = '\0';

  if (stat(lf, &st) < 0) {
    int xerrno = errno;
    pr_log_debug(DEBUG0, "error: unable to stat() %s: %s", lf,
      strerror(errno));
    destroy_pool(tmp_pool);

    errno = xerrno;
    return -1;
  }

  /* The path must be in a valid directory */
  if (!S_ISDIR(st.st_mode)) {
    pr_log_debug(DEBUG0, "error: %s is not a directory", lf);
    destroy_pool(tmp_pool);

    errno = ENOTDIR;
    return -1;
  }

  /* Do not log to world-writeable directories */
  if (st.st_mode & S_IWOTH) {
    pr_log_pri(PR_LOG_NOTICE, "error: %s is a world writeable directory", lf);
    destroy_pool(tmp_pool);
    return PR_LOG_WRITABLE_DIR;
  }

  /* Restore the path separator so that checks on the file itself may be
   * done.
   */
  *tmp = '/';

  allow_log_symlinks = get_param_ptr(main_server->conf, "AllowLogSymlinks",
    FALSE);

  if (allow_log_symlinks == NULL ||
      *allow_log_symlinks == FALSE) {
    int flags = O_APPEND|O_CREAT|O_WRONLY;

#ifdef PR_USE_NONBLOCKING_LOG_OPEN
    /* Use the O_NONBLOCK flag when opening log files, as they might be
     * FIFOs whose other end is not currently running; we do not want to
     * block indefinitely in such cases.
     */
    flags |= O_NONBLOCK;
#endif /* PR_USE_NONBLOCKING_LOG_OPEN */

#ifdef O_NOFOLLOW
    /* On systems that support the O_NOFOLLOW flag (e.g. Linux and FreeBSD),
     * use it so that the path being opened, if it is a symlink, is not
     * followed.
     */
    flags |= O_NOFOLLOW;

#elif defined(SOLARIS2)
    /* Solaris doesn't support the O_NOFOLLOW flag.  Instead, in their
     * wisdom (hah!), Solaris decided that if the given path is a symlink
     * and the flags O_CREAT and O_EXCL are set, the link is not followed.
     * Right.  The problem here is the case where the path is not a symlink;
     * using O_CREAT|O_EXCL will then cause the open() to fail if the
     * file already exists.
     */
    flags |= O_EXCL;
#endif /* O_NOFOLLOW or SOLARIS2 */

    *log_fd = open(lf, flags, log_mode);
    if (*log_fd < 0) {

      if (errno != EEXIST) {
        destroy_pool(tmp_pool);

        /* More portability fun: Linux likes to report ELOOP if O_NOFOLLOW
         * is used to open a symlink file; FreeBSD likes to return EMLINK.
         * Both would lead to rather misleading error messages being
         * logged.  Catch these errnos, and return the value that properly
         * informs the caller that the given path was an illegal symlink.
         */

        switch (errno) {
#ifdef ELOOP
          case ELOOP:
            return PR_LOG_SYMLINK;
#endif /* ELOOP */

#ifdef EMLINK
          case EMLINK:
            return PR_LOG_SYMLINK;
#endif /* EMLINK */
        }

        return -1;

      } else {
#if defined(SOLARIS2)
        /* On Solaris, because of the stupid multiplexing of O_CREAT and
         * O_EXCL to get open() not to follow a symlink, it's possible that
         * the path already exists.  Now, we'll try to open() without
         * O_EXCL, then lstat() the path to see if this pre-existing file is
         * a symlink or a regular file.
         *
         * Note that because this check cannot be done atomically on Solaris,
         * the possibility of a race condition/symlink attack still exists.
         * Solaris doesn't provide a good way around this situation.
         */
        flags &= ~O_EXCL;

        *log_fd = open(lf, flags, log_mode);
        if (*log_fd < 0) {
          destroy_pool(tmp_pool);
          return -1;
        }

        /* The race condition on Solaris is here, between the open() call
         * above and the lstat() call below...
         */

        if (lstat(lf, &st) != -1)
          have_stat = TRUE;
#else
        destroy_pool(tmp_pool);
        return -1;
#endif /* SOLARIS2 */
      }
    }

    /* Stat the file using the descriptor, not the path */
    if (!have_stat &&
        fstat(*log_fd, &st) != -1)
      have_stat = TRUE;

    if (!have_stat ||
        S_ISLNK(st.st_mode)) {
      pr_log_debug(DEBUG0, !have_stat ? "error: unable to stat %s" :
        "error: %s is a symbolic link", lf);

      close(*log_fd);
      *log_fd = -1;
      destroy_pool(tmp_pool);
      return PR_LOG_SYMLINK;
    }

  } else {
    int flags = O_CREAT|O_APPEND|O_WRONLY;

#ifdef PR_USE_NONBLOCKING_LOG_OPEN
    /* Use the O_NONBLOCK flag when opening log files, as they might be
     * FIFOs whose other end is not currently running; we do not want to
     * block indefinitely in such cases.
     */
    flags |= O_NONBLOCK;
#endif /* PR_USE_NONBLOCKING_LOG_OPEN */

    *log_fd = open(lf, flags, log_mode);
    if (*log_fd < 0) {
      destroy_pool(tmp_pool);
      return -1;
    }
  }

  /* Find a usable fd for the just-opened log fd. */
  if (*log_fd <= STDERR_FILENO) {
    res = pr_fs_get_usable_fd(*log_fd);
    if (res < 0) {
      pr_log_debug(DEBUG0, "warning: unable to find good fd for logfd %d: %s",
        *log_fd, strerror(errno));

    } else {
      close(*log_fd);
      *log_fd = res;
    }
  }

  if (fcntl(*log_fd, F_SETFD, FD_CLOEXEC) < 0) {
    pr_log_pri(PR_LOG_WARNING, "unable to set CLO_EXEC on log fd %d: %s",
      *log_fd, strerror(errno));
  }

#ifdef PR_USE_NONBLOCKING_LOG_OPEN
  /* Return the fd to blocking mode. */
  fd_set_block(*log_fd);
#endif /* PR_USE_NONBLOCKING_LOG_OPEN */

  destroy_pool(tmp_pool);
  return 0;
}

int pr_log_vwritefile(int logfd, const char *ident, const char *fmt,
    va_list msg) {
  char buf[PR_TUNABLE_BUFFER_SIZE] = {'\0'};
  time_t timestamp;
  struct tm *t = NULL;
  size_t buflen;

  if (logfd < 0) {
    errno = EINVAL;
    return -1;
  }

  timestamp = time(NULL);

  t = pr_localtime(NULL, &timestamp);
  if (t == NULL) {
    return -1;
  }

  /* Prepend the timestamp */
  strftime(buf, sizeof(buf), "%b %d %H:%M:%S ", t);
  buf[sizeof(buf)-1] = '\0';

  /* Prepend a small header */
  buflen = strlen(buf);
  snprintf(buf + buflen, sizeof(buf) - buflen, "%s[%u]: ", ident,
    (unsigned int) (session.pid ? session.pid : getpid()));
  buf[sizeof(buf)-1] = '\0';

  /* Affix the message */
  buflen = strlen(buf);
  vsnprintf(buf + buflen, sizeof(buf) - buflen - 1, fmt, msg);

  buf[sizeof(buf)-1] = '\0';

  buflen = strlen(buf);
  if (buflen < (sizeof(buf) - 1)) {
    buf[buflen] = '\n';

  } else {
    buf[sizeof(buf)-2] = '\n';
  }

  buflen = strlen(buf);

  pr_log_event_generate(PR_LOG_TYPE_UNSPEC, logfd, -1, buf, buflen);

  while (write(logfd, buf, buflen) < 0) {
    if (errno == EINTR) {
      pr_signals_handle();
      continue;
    }

    return -1;
  }

  return 0;
}

int pr_log_writefile(int logfd, const char *ident, const char *fmt, ...) {
  va_list msg;
  int res;

  if (logfd < 0) {
    errno = EINVAL;
    return -1;
  }

  va_start(msg, fmt);
  res = pr_log_vwritefile(logfd, ident, fmt, msg);
  va_end(msg);

  return res;
}

int log_opensyslog(const char *fn) {
  int res = 0;

  if (set_facility != -1)
    facility = set_facility;

  if (fn) {
    memset(systemlog_fn, '\0', sizeof(systemlog_fn));
    sstrncpy(systemlog_fn, fn, sizeof(systemlog_fn));
  }

  if (!*systemlog_fn) {

    /* The child may have inherited a valid socket from the parent. */
    pr_closelog(syslog_sockfd);

    syslog_sockfd = pr_openlog("proftpd", LOG_NDELAY|LOG_PID, facility);
    if (syslog_sockfd < 0)
      return -1;

    fcntl(syslog_sockfd, F_SETFD, FD_CLOEXEC);

    systemlog_fd = -1;

  } else if ((res = pr_log_openfile(systemlog_fn, &systemlog_fd,
      PR_LOG_SYSTEM_MODE)) < 0) {
    memset(systemlog_fn, '\0', sizeof(systemlog_fn));
    return res;
  }

  syslog_open = TRUE;
  return 0;
}

void log_closesyslog(void) {
  (void) close(systemlog_fd);
  systemlog_fd = -1;

  (void) pr_closelog(syslog_sockfd);
  syslog_sockfd = -1;

  syslog_open = FALSE;
}

int log_getfacility(void) {
  return set_facility;
}

void log_setfacility(int f) {
  set_facility = f;
}

void log_discard(void) {
  syslog_discard = TRUE;
}

static void log_write(int priority, int f, char *s, int discard) {
  unsigned int *max_priority = NULL;
  char serverinfo[PR_TUNABLE_BUFFER_SIZE] = {'\0'};

  memset(serverinfo, '\0', sizeof(serverinfo));

  if (main_server &&
      main_server->ServerFQDN) {
    pr_netaddr_t *remote_addr = pr_netaddr_get_sess_remote_addr();
    const char *remote_name = pr_netaddr_get_sess_remote_name();

    snprintf(serverinfo, sizeof(serverinfo), "%s", main_server->ServerFQDN);
    serverinfo[sizeof(serverinfo)-1] = '\0';

    if (remote_addr && remote_name) {
      snprintf(serverinfo + strlen(serverinfo),
        sizeof(serverinfo) - strlen(serverinfo), " (%s[%s])",
        remote_name, pr_netaddr_get_ipstr(remote_addr));

      serverinfo[sizeof(serverinfo)-1] = '\0';
    }
  }

  if (!discard &&
      (logstderr || !main_server)) {
    char buf[LOGBUFFER_SIZE] = {'\0'};
    size_t buflen;
    time_t now = time(NULL);
    struct tm *tm = NULL;

    tm = pr_localtime(NULL, &now);
    if (tm == NULL) {
      return;
    }

    strftime(buf, sizeof(buf)-1, "%b %d %H:%M:%S ", tm);
    buf[sizeof(buf)-1] = '\0';
    buflen = strlen(buf);

    if (*serverinfo) {
      snprintf(buf + buflen, sizeof(buf) - buflen,
               "%s proftpd[%u] %s: %s\n", systemlog_host,
               (unsigned int) (session.pid ? session.pid : getpid()),
               serverinfo, s);
    } else {
      snprintf(buf + buflen, sizeof(buf) - buflen,
               "%s proftpd[%u]: %s\n", systemlog_host,
               (unsigned int) (session.pid ? session.pid : getpid()), s);
    }

    buf[sizeof(buf)-1] = '\0';
    buflen = strlen(buf);

    pr_log_event_generate(PR_LOG_TYPE_SYSTEMLOG, STDERR_FILENO, priority,
      buf, buflen);

    fprintf(stderr, "%s", buf);
    return;
  }

  if (syslog_discard) {
    /* Only return now if we don't have any log listeners. */
    if (pr_log_event_listening(PR_LOG_TYPE_SYSLOG) == FALSE &&
        pr_log_event_listening(PR_LOG_TYPE_SYSTEMLOG) == FALSE) {
      return;
    }
  }

  max_priority = get_param_ptr(main_server->conf, "SyslogLevel", FALSE);
  if (max_priority != NULL &&
      priority > *max_priority) {

    /* Only return now if we don't have any log listeners. */
    if (pr_log_event_listening(PR_LOG_TYPE_SYSLOG) == FALSE &&
        pr_log_event_listening(PR_LOG_TYPE_SYSTEMLOG) == FALSE) {
      return;
    }
  }

  if (systemlog_fd != -1) {
    char buf[LOGBUFFER_SIZE] = {'\0'};
    size_t buflen;
    time_t tt = time(NULL);
    struct tm *t;

    t = pr_localtime(NULL, &tt);
    if (t == NULL) {
      return;
    }

    strftime(buf, sizeof(buf), "%b %d %H:%M:%S ", t);
    buf[sizeof(buf) - 1] = '\0';

    if (*serverinfo) {
      snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
	       "%s proftpd[%u] %s: %s\n", systemlog_host,
	       (unsigned int) (session.pid ? session.pid : getpid()),
               serverinfo, s);
    } else {
      snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
	       "%s proftpd[%u]: %s\n", systemlog_host,
	       (unsigned int) (session.pid ? session.pid : getpid()), s);
    }

    buf[sizeof(buf) - 1] = '\0';
    buflen = strlen(buf);

    pr_log_event_generate(PR_LOG_TYPE_SYSTEMLOG, systemlog_fd, priority,
      buf, buflen);

    /* Now we need to enforce the discard, syslog_discard and SyslogLevel
     * filtering.
     */
    if (discard) {
      return;
    }

    if (syslog_discard) {
      return;
    }

    if (max_priority != NULL &&
        priority > *max_priority) {
      return;
    }

    while (write(systemlog_fd, buf, buflen) < 0) {
      if (errno == EINTR) {
        pr_signals_handle();
        continue;
      }

      return;
    }

    return;
  }

  pr_log_event_generate(PR_LOG_TYPE_SYSLOG, syslog_sockfd, priority, s,
    strlen(s));

  if (set_facility != -1)
    f = set_facility;

  if (!syslog_open) {
    syslog_sockfd = pr_openlog("proftpd", LOG_NDELAY|LOG_PID, f);
    syslog_open = TRUE;

  } else if (f != facility) {
    /* If this message is to be sent to a different log facility than a
     * default one (or the facility configured via SyslogFacility), then
     * OR in the facility with the priority value, as per the syslog(3)
     * docs.
     */
    priority |= f;
  }

  if (*serverinfo) {
    pr_syslog(syslog_sockfd, priority, "%s - %s\n", serverinfo, s);

  } else {
    pr_syslog(syslog_sockfd, priority, "%s\n", s);
  }
}

void pr_log_pri(int priority, const char *fmt, ...) {
  char buf[LOGBUFFER_SIZE] = {'\0'};
  va_list msg;

  va_start(msg, fmt);
  vsnprintf(buf, sizeof(buf), fmt, msg);
  va_end(msg);

  /* Always make sure the buffer is NUL-terminated. */
  buf[sizeof(buf) - 1] = '\0';

  log_write(priority, facility, buf, FALSE);
}

/* Like pr_log_pri(), but sends the log entry in the LOG_AUTHPRIV
 * facility (presumably it doesn't need to be seen by everyone).
 */
void pr_log_auth(int priority, const char *fmt, ...) {
  char buf[LOGBUFFER_SIZE] = {'\0'};
  va_list msg;

  va_start(msg, fmt);
  vsnprintf(buf, sizeof(buf), fmt, msg);
  va_end(msg);

  /* Always make sure the buffer is NUL-terminated. */
  buf[sizeof(buf) - 1] = '\0';

  log_write(priority, LOG_AUTHPRIV, buf, FALSE);
}

/* Disable logging to stderr, should be done right before forking
 * or disassociation from controlling tty.  After disabling stderr
 * logging, all messages go to syslog.
 */
void log_stderr(int bool) {
  logstderr = bool;
}

/* Set the debug logging level, see log.h for constants.  Higher
 * numbers mean print more, DEBUG0 (0) == print no debugging log
 * (default)
 */
int pr_log_setdebuglevel(int level) {
  int old_level = debug_level;
  debug_level = level;
  return old_level;
}

/* Convert a string into the matching syslog level value.  Return -1
 * if no matching level is found.
 */
int pr_log_str2sysloglevel(const char *name) {

  if (strncasecmp(name, "emerg", 6) == 0) {
    return PR_LOG_EMERG;

  } else if (strncasecmp(name, "alert", 6) == 0) {
    return PR_LOG_ALERT;

  } else if (strncasecmp(name, "crit", 5) == 0) {
    return PR_LOG_CRIT;

  } else if (strncasecmp(name, "error", 6) == 0) {
    return PR_LOG_ERR;

  } else if (strncasecmp(name, "warn", 5) == 0) {
    return PR_LOG_WARNING;

  } else if (strncasecmp(name, "notice", 7) == 0) {
    return PR_LOG_NOTICE;

  } else if (strncasecmp(name, "info", 5) == 0) {
    return PR_LOG_INFO;

  } else if (strncasecmp(name, "debug", 6) == 0) {
    return PR_LOG_DEBUG;
  }

  errno = ENOENT;
  return -1;
}

void pr_log_debug(int level, const char *fmt, ...) {
  char buf[LOGBUFFER_SIZE] = {'\0'};
  va_list msg;
  int discard = FALSE;

  if (debug_level < level) {
    discard = TRUE;

    if (pr_log_event_listening(PR_LOG_TYPE_SYSLOG) == FALSE &&
        pr_log_event_listening(PR_LOG_TYPE_SYSTEMLOG) == FALSE) {
      return;
    }
  }

  if (fmt == NULL)
    return;

  memset(buf, '\0', sizeof(buf));
  va_start(msg, fmt);
  vsnprintf(buf, sizeof(buf), fmt, msg);
  va_end(msg);

  /* Always make sure the buffer is NUL-terminated. */
  buf[sizeof(buf) - 1] = '\0';

  log_write(PR_LOG_DEBUG, facility, buf, discard);
}

static const char *get_log_event_name(unsigned int log_type) {
  const char *event_name = NULL;

  switch (log_type) {
    case PR_LOG_TYPE_UNSPEC:
      event_name = "core.log.unspec";
      break;

    case PR_LOG_TYPE_XFERLOG:
      event_name = "core.log.xferlog";
      break;

    case PR_LOG_TYPE_SYSLOG:
      event_name = "core.log.syslog"; 
      break;

    case PR_LOG_TYPE_SYSTEMLOG:
      event_name = "core.log.systemlog"; 
      break;

    case PR_LOG_TYPE_EXTLOG:
      event_name = "core.log.extendedlog"; 
      break;

    case PR_LOG_TYPE_TRACELOG:
      event_name = "core.log.tracelog"; 
      break;

    default:
      errno = EINVAL;
      return NULL;
  }

  return event_name;
}

int pr_log_event_generate(unsigned int log_type, int log_fd, int log_level,
    const char *log_msg, size_t log_msglen) {
  const char *event_name;
  pr_log_event_t le;

  if (log_msg == NULL ||
      log_msglen == 0) {
    errno = EINVAL;
    return -1;
  }

  if (pr_log_event_listening(log_type) == FALSE) {
    errno = ENOENT;
    return -1;
  }

  event_name = get_log_event_name(log_type);

  memset(&le, 0, sizeof(le));
  le.log_type = log_type;
  le.log_fd = log_fd;
  le.log_level = log_level;
  le.log_msg = log_msg;
  le.log_msglen = log_msglen;

  pr_event_generate(event_name, &le);
  return 0;
}

int pr_log_event_listening(unsigned int log_type) {
  const char *event_name;
  int res;

  event_name = get_log_event_name(log_type);
  if (event_name == NULL) {
    return FALSE;
  }

  res = pr_event_listening(event_name);
  if (res <= 0) {
    return FALSE;
  }

  return TRUE;
}

void init_log(void) {
  char buf[256];

  memset(buf, '\0', sizeof(buf));
  if (gethostname(buf, sizeof(buf)) == -1)
    sstrncpy(buf, "localhost", sizeof(buf));

  sstrncpy(systemlog_host, (char *) pr_netaddr_validate_dns_str(buf),
    sizeof(systemlog_host));
  memset(systemlog_fn, '\0', sizeof(systemlog_fn));
  log_closesyslog();
}
