/* Auffangnetz für Systemaufrufe, die Android einer App per seccomp verbietet
   (glibc des armv7-Root-FS von Winlator).

   Android legt ab 8.0 jedem App-Prozess einen seccomp-Filter an; er gilt auch für alles, was
   die App startet (Box86, Wine, wineserver).  Nicht erlaubte Systemaufrufe beantwortet der
   Kernel mit SIGSYS (SECCOMP_RET_TRAP), statt ENOSYS zu liefern, und SIGSYS beendet das
   Programm.  glibc rechnet bei neueren Aufrufen mit ENOSYS und weicht dann auf ältere aus;
   auf Android 8 und 9 sind aber auch alle time64-Aufrufe (ab Linux 5.1) gesperrt, ebenso
   einige alte 32-Bit-Aufrufe (link, lchown32, pause, ...), die glibc auf ARM direkt nutzt.

   Dieser Handler wird beim Laden von libc in jedem Prozess eingerichtet.  Er bildet die alten
   Aufrufe auf erlaubte Gegenstücke ab (link -> linkat usw.) und lässt alle übrigen gesperrten
   Aufrufe mit -ENOSYS zurückkehren, als kenne der Kernel sie nicht.  Jeder gesperrte Aufruf
   wird einmal pro Prozess auf stderr gemeldet ("winlator-seccomp: ...").  Programme, die
   SIGSYS selbst behandeln, ersetzen den Handler wie gewohnt.  Auf Geräten ohne seccomp-Filter
   (Android 7) tritt kein SIGSYS auf; der Handler bleibt dann unbenutzt.  */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/ucontext.h>
#include <sysdep.h>
#include <unistd.h>

#ifndef SYS_SECCOMP
# define SYS_SECCOMP 1
#endif

/* ARM-EABI-Nummern der Aufrufe, die Android 8/9 sperren (unabhängig davon, ob glibc sie
   in arch-syscall.h noch kennt)  */
enum
{
  WS_link = 9, WS_mknod = 14, WS_chmod = 15, WS_lchown16 = 16, WS_pause = 29, WS_rmdir = 40,
  WS_getpgrp = 65, WS_symlink = 83, WS_ftruncate = 93, WS_fchown16 = 95, WS_chown16 = 182,
  WS_lchown32 = 198, WS_chown32 = 212, WS_utimes = 269, WS_accept = 285, WS_send = 289,
  WS_recv = 291, WS_futimesat = 326, WS_signalfd = 349,
};

static const char *ws_progname = "?";
static unsigned int ws_reported[1024 / 32];

static const char *
ws_name (int nr)
{
  switch (nr)
    {
    case 9: return "link";
    case 14: return "mknod";
    case 15: return "chmod";
    case 16: return "lchown";
    case 29: return "pause";
    case 34: return "nice";
    case 40: return "rmdir";
    case 47: return "getgid";
    case 49: return "geteuid";
    case 50: return "getegid";
    case 65: return "getpgrp";
    case 72: return "sigsuspend";
    case 73: return "sigpending";
    case 80: return "getgroups";
    case 83: return "symlink";
    case 93: return "ftruncate";
    case 95: return "fchown";
    case 99: return "statfs";
    case 100: return "fstatfs";
    case 106: return "stat";
    case 107: return "lstat";
    case 108: return "fstat";
    case 126: return "sigprocmask";
    case 182: return "chown";
    case 198: return "lchown32";
    case 203: return "setreuid32";
    case 206: return "setgroups32";
    case 210: return "setresgid32";
    case 212: return "chown32";
    case 213: return "setuid32";
    case 214: return "setgid32";
    case 269: return "utimes";
    case 274: return "mq_open";
    case 285: return "accept";
    case 289: return "send";
    case 291: return "recv";
    case 298: return "semop";
    case 299: return "semget";
    case 300: return "semctl";
    case 301: return "msgsnd";
    case 302: return "msgrcv";
    case 303: return "msgget";
    case 304: return "msgctl";
    case 305: return "shmat";
    case 306: return "shmdt";
    case 307: return "shmget";
    case 308: return "shmctl";
    case 309: return "add_key";
    case 310: return "request_key";
    case 311: return "keyctl";
    case 312: return "semtimedop";
    case 314: return "ioprio_set";
    case 315: return "ioprio_get";
    case 319: return "mbind";
    case 320: return "get_mempolicy";
    case 321: return "set_mempolicy";
    case 326: return "futimesat";
    case 338: return "set_robust_list";
    case 339: return "get_robust_list";
    case 349: return "signalfd";
    case 367: return "fanotify_init";
    case 368: return "fanotify_mark";
    case 370: return "name_to_handle_at";
    case 371: return "open_by_handle_at";
    case 378: return "kcmp";
    case 386: return "bpf";
    case 388: return "userfaultfd";
    case 389: return "membarrier";
    case 394: return "pkey_mprotect";
    case 395: return "pkey_alloc";
    case 396: return "pkey_free";
    case 397: return "statx";
    case 398: return "rseq";
    case 399: return "io_pgetevents";
    case 403: return "clock_gettime64";
    case 404: return "clock_settime64";
    case 405: return "clock_adjtime64";
    case 406: return "clock_getres_time64";
    case 407: return "clock_nanosleep_time64";
    case 408: return "timer_gettime64";
    case 409: return "timer_settime64";
    case 410: return "timerfd_gettime64";
    case 411: return "timerfd_settime64";
    case 412: return "utimensat_time64";
    case 413: return "pselect6_time64";
    case 414: return "ppoll_time64";
    case 416: return "io_pgetevents_time64";
    case 417: return "recvmmsg_time64";
    case 418: return "mq_timedsend_time64";
    case 419: return "mq_timedreceive_time64";
    case 420: return "semtimedop_time64";
    case 421: return "rt_sigtimedwait_time64";
    case 422: return "futex_time64";
    case 423: return "sched_rr_get_interval_time64";
    case 424: return "pidfd_send_signal";
    case 425: return "io_uring_setup";
    case 426: return "io_uring_enter";
    case 427: return "io_uring_register";
    case 434: return "pidfd_open";
    case 435: return "clone3";
    case 436: return "close_range";
    case 437: return "openat2";
    case 438: return "pidfd_getfd";
    case 439: return "faccessat2";
    case 440: return "process_madvise";
    case 441: return "epoll_pwait2";
    case 443: return "quotactl_fd";
    case 444: return "landlock_create_ruleset";
    case 445: return "landlock_add_rule";
    case 446: return "landlock_restrict_self";
    case 448: return "process_mrelease";
    case 449: return "futex_waitv";
    case 451: return "cachestat";
    case 452: return "fchmodat2";
    case 454: return "futex_wake";
    case 455: return "futex_wait";
    case 456: return "futex_requeue";
    case 457: return "statmount";
    case 458: return "listmount";
    case 462: return "mseal";
    default: return NULL;
    }
}

/* 16-Bit-IDs der alten Aufrufe: 0xffff heißt "nicht ändern" */
static inline unsigned long int
ws_id16 (unsigned long int id)
{
  id &= 0xffff;
  return id == 0xffff ? (unsigned long int) -1 : id;
}

/* Kernel-ABI der alten 32-Bit-Aufrufe, unabhängig davon, mit welchem time_t libc gebaut ist */
struct ws_timeval32 { int tv_sec; int tv_usec; };
struct ws_timespec32 { int tv_sec; int tv_nsec; };

/* timeval[2] nach timespec[2] für utimensat; NULL bleibt NULL */
static const struct ws_timespec32 *
ws_timevals (unsigned long int arg, struct ws_timespec32 ts[2])
{
  const struct ws_timeval32 *tv = (const struct ws_timeval32 *) arg;
  if (tv == NULL)
    return NULL;
  for (int i = 0; i < 2; i++)
    {
      ts[i].tv_sec = tv[i].tv_sec;
      ts[i].tv_nsec = tv[i].tv_usec * 1000;
    }
  return ts;
}

/* Gesperrten Aufruf ausführen, soweit es ein erlaubtes Gegenstück gibt; sonst -ENOSYS.
   *how bekommt den Namen des Ersatzes (für die Meldung).  */
static long int
ws_emulate (int nr, const unsigned long int *a, const sigset_t *mask, const char **how)
{
  struct ws_timespec32 ts[2];
  switch (nr)
    {
    case WS_link:
      *how = "linkat";
      return INTERNAL_SYSCALL_CALL (linkat, AT_FDCWD, a[0], AT_FDCWD, a[1], 0);
    case WS_mknod:
      *how = "mknodat";
      return INTERNAL_SYSCALL_CALL (mknodat, AT_FDCWD, a[0], a[1], a[2]);
    case WS_chmod:
      *how = "fchmodat";
      return INTERNAL_SYSCALL_CALL (fchmodat, AT_FDCWD, a[0], a[1]);
    case WS_rmdir:
      *how = "unlinkat";
      return INTERNAL_SYSCALL_CALL (unlinkat, AT_FDCWD, a[0], AT_REMOVEDIR);
    case WS_symlink:
      *how = "symlinkat";
      return INTERNAL_SYSCALL_CALL (symlinkat, a[0], AT_FDCWD, a[1]);
    case WS_chown32:
      *how = "fchownat";
      return INTERNAL_SYSCALL_CALL (fchownat, AT_FDCWD, a[0], a[1], a[2], 0);
    case WS_lchown32:
      *how = "fchownat";
      return INTERNAL_SYSCALL_CALL (fchownat, AT_FDCWD, a[0], a[1], a[2], AT_SYMLINK_NOFOLLOW);
    case WS_chown16:
      *how = "fchownat";
      return INTERNAL_SYSCALL_CALL (fchownat, AT_FDCWD, a[0], ws_id16 (a[1]), ws_id16 (a[2]), 0);
    case WS_lchown16:
      *how = "fchownat";
      return INTERNAL_SYSCALL_CALL (fchownat, AT_FDCWD, a[0], ws_id16 (a[1]), ws_id16 (a[2]),
				    AT_SYMLINK_NOFOLLOW);
    case WS_fchown16:
      *how = "fchown32";
      return INTERNAL_SYSCALL_CALL (fchown32, a[0], ws_id16 (a[1]), ws_id16 (a[2]));
    case WS_ftruncate:
      /* ARM-EABI: 64-Bit-Argument in einem geraden Registerpaar, davor ein Füllwert */
      *how = "ftruncate64";
      return INTERNAL_SYSCALL_CALL (ftruncate64, a[0], 0, a[1], (long int) a[1] < 0 ? -1 : 0);
    case WS_pause:
      /* wartet wie pause() mit der Signalmaske des unterbrochenen Codes auf ein Signal und
	 kehrt nach dessen Handler mit -EINTR zurück */
      *how = "rt_sigsuspend";
      return INTERNAL_SYSCALL_CALL (rt_sigsuspend, mask, __NSIG_BYTES);
    case WS_getpgrp:
      *how = "getpgid";
      return INTERNAL_SYSCALL_CALL (getpgid, 0);
    case WS_utimes:
      *how = "utimensat";
      return INTERNAL_SYSCALL_CALL (utimensat, AT_FDCWD, a[0], ws_timevals (a[1], ts), 0);
    case WS_futimesat:
      *how = "utimensat";
      return INTERNAL_SYSCALL_CALL (utimensat, a[0], a[1], ws_timevals (a[2], ts), 0);
    case WS_accept:
      *how = "accept4";
      return INTERNAL_SYSCALL_CALL (accept4, a[0], a[1], a[2], 0);
    case WS_send:
      *how = "sendto";
      return INTERNAL_SYSCALL_CALL (sendto, a[0], a[1], a[2], a[3], 0, 0);
    case WS_recv:
      *how = "recvfrom";
      return INTERNAL_SYSCALL_CALL (recvfrom, a[0], a[1], a[2], a[3], 0, 0);
    case WS_signalfd:
      *how = "signalfd4";
      return INTERNAL_SYSCALL_CALL (signalfd4, a[0], a[1], a[2], 0);
    }
  *how = NULL;
  return -ENOSYS;
}

static char *
ws_append (char *p, char *end, const char *s)
{
  while (*s != '\0' && p < end)
    *p++ = *s++;
  return p;
}

static char *
ws_append_num (char *p, char *end, unsigned int v)
{
  char digits[12];
  int n = 0;
  do
    digits[n++] = '0' + v % 10;
  while ((v /= 10) != 0);
  while (n > 0 && p < end)
    *p++ = digits[--n];
  return p;
}

static void
ws_report (int nr, const char *how)
{
  unsigned int bit = (unsigned int) nr;
  if (bit < 1024)
    {
      unsigned int mask = 1u << (bit % 32);
      if (ws_reported[bit / 32] & mask)
	return;
      ws_reported[bit / 32] |= mask;
    }
  char buf[200];
  char *end = buf + sizeof buf - 1, *p = buf;
  p = ws_append (p, end, "winlator-seccomp: ");
  p = ws_append (p, end, ws_progname);
  p = ws_append (p, end, ": Systemaufruf ");
  p = ws_append_num (p, end, (unsigned int) nr);
  const char *name = ws_name (nr);
  if (name != NULL)
    {
      p = ws_append (p, end, " (");
      p = ws_append (p, end, name);
      p = ws_append (p, end, ")");
    }
  p = ws_append (p, end, " von Android gesperrt, ");
  if (how != NULL)
    {
      p = ws_append (p, end, "ersetzt durch ");
      p = ws_append (p, end, how);
    }
  else
    p = ws_append (p, end, "liefert ENOSYS");
  *p++ = '\n';
  INTERNAL_SYSCALL_CALL (write, STDERR_FILENO, buf, p - buf);
}

static void
ws_sigsys (int sig, siginfo_t *info, void *context)
{
  if (info == NULL || info->si_code != SYS_SECCOMP || context == NULL)
    {
      /* echtes SIGSYS (kill, raise): wie ohne Handler beenden */
      struct sigaction dfl;
      memset (&dfl, 0, sizeof dfl);
      dfl.sa_handler = SIG_DFL;
      __sigaction (SIGSYS, &dfl, NULL);
      int pid = INTERNAL_SYSCALL_CALL (getpid);
      INTERNAL_SYSCALL_CALL (kill, pid, SIGSYS);
      return;
    }

  ucontext_t *uc = context;
  /* r0..r5 liegen in mcontext_t direkt hintereinander; r0 hat der Kernel beim Abfangen
     wieder auf das erste Argument gesetzt */
  unsigned long int *regs = &uc->uc_mcontext.arm_r0;
  unsigned long int args[6] = { regs[0], regs[1], regs[2], regs[3], regs[4], regs[5] };
  const char *how;
  int nr = info->si_syscall;
  long int ret = ws_emulate (nr, args, &uc->uc_sigmask, &how);
  ws_report (nr, how);
  /* Rückgabewert des Aufrufs: nach dem Handler geht es hinter dem svc-Befehl weiter */
  regs[0] = (unsigned long int) ret;
}

static void
__attribute__ ((constructor))
ws_init (int argc, char **argv, char **envp)
{
  if (argc > 0 && argv != NULL && argv[0] != NULL)
    {
      const char *slash = strrchr (argv[0], '/');
      ws_progname = slash != NULL ? slash + 1 : argv[0];
    }
  struct sigaction sa;
  memset (&sa, 0, sizeof sa);
  sa.sa_sigaction = ws_sigsys;
  sa.sa_flags = SA_SIGINFO;
  __sigaction (SIGSYS, &sa, NULL);
}
