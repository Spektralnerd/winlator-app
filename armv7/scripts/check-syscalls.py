#!/usr/bin/env python3
"""Sucht in ARM-ELF-Dateien (glibc) nach Systemaufrufen, die Android 8/9 Apps verbieten.

Android legt App-Prozessen ab 8.0 einen seccomp-Filter an; gesperrte Aufrufe beenden das
Programm mit SIGSYS.  Die Tabelle unten ist der Filter für 32-Bit-ARM-Apps aus bionic
(LineageOS 15.1 = Android 8.1 und 16.0 = Android 9, libc/seccomp/arm_*policy.cpp), dazu alle
Aufrufe ab Linux 4.14, die dort fehlen (time64, clone3 usw.).

Gezählt werden Konstanten, die in ip (Thumb-glibc: bl __libc_do_syscall) oder r7 (svc 0)
geladen werden.  Aufrufe über syscall() mit variabler Nummer sieht die Heuristik nicht.

Aufruf: check-syscalls.py [--forbid 9,29] [--strict] <elf>...
  --forbid   diese Nummern dürfen nirgends direkt vorkommen (Fehler)
  --strict   gar kein gesperrter Aufruf erlaubt (für ld.so: läuft vor dem SIGSYS-Handler)
"""
import re
import subprocess
import sys

BLOCKED = {
    9: "link", 14: "mknod", 15: "chmod", 16: "lchown", 21: "mount", 23: "setuid", 29: "pause",
    34: "nice", 40: "rmdir", 46: "setgid", 47: "getgid", 49: "geteuid", 50: "getegid", 51: "acct",
    52: "umount2", 61: "chroot", 62: "ustat", 65: "getpgrp", 70: "setreuid", 71: "setregid",
    72: "sigsuspend", 73: "sigpending", 74: "sethostname", 79: "settimeofday", 80: "getgroups",
    81: "setgroups", 83: "symlink", 86: "uselib", 87: "swapon", 88: "reboot", 93: "ftruncate",
    95: "fchown", 99: "statfs", 100: "fstatfs", 103: "syslog", 106: "stat", 107: "lstat",
    108: "fstat", 111: "vhangup", 115: "swapoff", 121: "setdomainname", 124: "adjtimex",
    126: "sigprocmask", 128: "init_module", 129: "delete_module", 134: "bdflush", 135: "sysfs",
    138: "setfsuid", 139: "setfsgid", 149: "_sysctl", 164: "setresuid", 165: "getresuid",
    169: "nfsservctl", 170: "setresgid", 171: "getresgid", 182: "chown", 198: "lchown32",
    203: "setreuid32", 206: "setgroups32", 210: "setresgid32", 212: "chown32", 213: "setuid32",
    214: "setgid32", 215: "setfsuid32", 216: "setfsgid32", 218: "pivot_root", 243: "io_setup",
    244: "io_destroy", 245: "io_getevents", 246: "io_submit", 247: "io_cancel",
    249: "lookup_dcookie", 262: "clock_settime", 269: "utimes", 271: "pciconfig_iobase",
    272: "pciconfig_read", 273: "pciconfig_write", 274: "mq_open", 275: "mq_unlink",
    276: "mq_timedsend", 277: "mq_timedreceive", 278: "mq_notify", 279: "mq_getsetattr",
    285: "accept", 289: "send", 291: "recv", 298: "semop", 299: "semget", 300: "semctl",
    301: "msgsnd", 302: "msgrcv", 303: "msgget", 304: "msgctl", 305: "shmat", 306: "shmdt",
    307: "shmget", 308: "shmctl", 309: "add_key", 310: "request_key", 311: "keyctl",
    312: "semtimedop", 313: "vserver", 314: "ioprio_set", 315: "ioprio_get", 319: "mbind",
    320: "get_mempolicy", 321: "set_mempolicy", 326: "futimesat", 338: "set_robust_list",
    339: "get_robust_list", 344: "move_pages", 347: "kexec_load", 349: "signalfd",
    367: "fanotify_init", 368: "fanotify_mark", 370: "name_to_handle_at",
    371: "open_by_handle_at", 372: "clock_adjtime", 378: "kcmp", 379: "finit_module", 386: "bpf",
    388: "userfaultfd", 389: "membarrier", 394: "pkey_mprotect", 395: "pkey_alloc",
    396: "pkey_free", 397: "statx", 398: "rseq", 399: "io_pgetevents", 400: "migrate_pages",
    401: "kexec_file_load", 403: "clock_gettime64", 404: "clock_settime64",
    405: "clock_adjtime64", 406: "clock_getres_time64", 407: "clock_nanosleep_time64",
    408: "timer_gettime64", 409: "timer_settime64", 410: "timerfd_gettime64",
    411: "timerfd_settime64", 412: "utimensat_time64", 413: "pselect6_time64",
    414: "ppoll_time64", 416: "io_pgetevents_time64", 417: "recvmmsg_time64",
    418: "mq_timedsend_time64", 419: "mq_timedreceive_time64", 420: "semtimedop_time64",
    421: "rt_sigtimedwait_time64", 422: "futex_time64", 423: "sched_rr_get_interval_time64",
    424: "pidfd_send_signal", 425: "io_uring_setup", 426: "io_uring_enter",
    427: "io_uring_register", 428: "open_tree", 429: "move_mount", 430: "fsopen", 431: "fsconfig",
    432: "fsmount", 433: "fspick", 434: "pidfd_open", 435: "clone3", 436: "close_range",
    437: "openat2", 438: "pidfd_getfd", 439: "faccessat2", 440: "process_madvise",
    441: "epoll_pwait2", 442: "mount_setattr", 443: "quotactl_fd", 444: "landlock_create_ruleset",
    445: "landlock_add_rule", 446: "landlock_restrict_self", 448: "process_mrelease",
    449: "futex_waitv", 450: "set_mempolicy_home_node", 451: "cachestat", 452: "fchmodat2",
    453: "map_shadow_stack", 454: "futex_wake", 455: "futex_wait", 456: "futex_requeue",
    457: "statmount", 458: "listmount", 459: "lsm_get_self_attr", 460: "lsm_set_self_attr",
    461: "lsm_list_modules", 462: "mseal",
}
# alles ab 394, was in den Tabellen fehlt, ist auf Android 8/9 ebenfalls gesperrt
for _nr in range(394, 470):
    BLOCKED.setdefault(_nr, "?")

IMM = re.compile(r"\b(?:movw|mov\.w|movs|mov)\s+(ip|r7),\s*#(\d+)")
FUNC = re.compile(r"^[0-9a-f]+ <(.+)>:$")


def scan(path, objdump):
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", path], capture_output=True, text=True,
                         check=True).stdout
    last = {"ip": None, "r7": None}
    func = "?"
    hits = {}
    for line in out.splitlines():
        m = FUNC.match(line)
        if m:
            func, last = m.group(1), {"ip": None, "r7": None}
            continue
        m = IMM.search(line)
        if m:
            last[m.group(1)] = int(m.group(2))
            continue
        if "__libc_do_syscall" in line and re.search(r"\bbl\b", line) and last["ip"] is not None:
            hits.setdefault(last["ip"], set()).add(func)
        elif re.search(r"\bsvc\s+(0x0+|0)\b", line) and last["r7"] is not None:
            hits.setdefault(last["r7"], set()).add(func)
    return hits


def main():
    args = sys.argv[1:]
    forbid, strict, objdump = set(), False, "arm-linux-gnueabihf-objdump"
    while args and args[0].startswith("--"):
        opt = args.pop(0)
        if opt == "--forbid":
            forbid = {int(x) for x in args.pop(0).split(",") if x}
        elif opt == "--strict":
            strict = True
        elif opt == "--objdump":
            objdump = args.pop(0)
    errors = 0
    for path in args:
        hits = scan(path, objdump)
        bad = {nr: f for nr, f in hits.items() if nr in BLOCKED}
        print(f"{path}: {len(hits)} Systemaufrufe, davon auf Android 8/9 gesperrt: {len(bad)}")
        for nr in sorted(bad):
            fatal = strict or nr in forbid
            errors += fatal
            where = ", ".join(sorted(bad[nr])[:4])
            print(f"  {'FEHLER' if fatal else 'Hinweis'}: {nr} {BLOCKED[nr]} in {where}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
