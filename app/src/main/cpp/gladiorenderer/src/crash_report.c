#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <signal.h>
#include <dlfcn.h>
#include <link.h>
#include <ucontext.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <stdint.h>

#include "crash_report.h"
#include "gl_context.h"
#include "gl_renderer.h"

extern void gladio_printLastRequests(GLContext* context, const char* prefix);

/* Zuordnung Thread -> Verbindung/Phase ohne TLS (im Signal-Handler darf nichts Speicher anfordern;
 * emulierte TLS auf Android vor API 29 täte das beim ersten Zugriff eines Threads) */
#define MAX_TRACKED_THREADS 64
static struct {
    volatile pid_t tid;
    GLContext* volatile context;
    const char* volatile phase;
} trackedThreads[MAX_TRACKED_THREADS];

static pid_t ownTid(void) {
    return (pid_t)syscall(SYS_gettid);
}

static int findSlot(pid_t tid, bool create) {
    for (int i = 0; i < MAX_TRACKED_THREADS; i++) if (trackedThreads[i].tid == tid) return i;
    if (!create) return -1;
    for (int i = 0; i < MAX_TRACKED_THREADS; i++) {
        if (__sync_bool_compare_and_swap(&trackedThreads[i].tid, 0, tid)) return i;
    }
    return -1;
}

static void releaseSlotIfUnused(int slot) {
    if (slot >= 0 && !trackedThreads[slot].context && !trackedThreads[slot].phase) trackedThreads[slot].tid = 0;
}

static const int crashSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
#define CRASH_SIGNAL_COUNT (int)(sizeof(crashSignals) / sizeof(crashSignals[0]))
static struct sigaction oldActions[CRASH_SIGNAL_COUNT];
static pthread_once_t installOnce = PTHREAD_ONCE_INIT;
static volatile int handling = 0;

/* ausführbare Bereiche der geladenen Bibliotheken (für die Suche nach Rücksprungadressen) */
#define MAX_CODE_RANGES 96
static struct {
    uintptr_t start, end;
    const char* name;
} codeRanges[MAX_CODE_RANGES];
static volatile int numCodeRanges = 0;
static pthread_mutex_t rangesMutex = PTHREAD_MUTEX_INITIALIZER;

static int collectRange(struct dl_phdr_info* info, size_t size, void* data) {
    int* count = data;
    for (int i = 0; i < info->dlpi_phnum && *count < MAX_CODE_RANGES; i++) {
        const ElfW(Phdr)* ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X)) continue;
        codeRanges[*count].start = info->dlpi_addr + ph->p_vaddr;
        codeRanges[*count].end = info->dlpi_addr + ph->p_vaddr + ph->p_memsz;
        codeRanges[*count].name = info->dlpi_name;
        (*count)++;
    }
    return 0;
}

/* außerhalb des Signal-Handlers (dl_iterate_phdr sperrt den Linker) */
static void refreshCodeRanges(void) {
    pthread_mutex_lock(&rangesMutex);
    int count = 0;
    numCodeRanges = 0;
    dl_iterate_phdr(collectRange, &count);
    numCodeRanges = count;
    pthread_mutex_unlock(&rangesMutex);
}

static bool isCodeAddress(uintptr_t addr) {
    int n = numCodeRanges;
    for (int i = 0; i < n; i++) {
        if (addr >= codeRanges[i].start && addr < codeRanges[i].end) return true;
    }
    return false;
}

static const char* baseName(const char* path) {
    if (!path) return "?";
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void printAddress(const char* label, uintptr_t addr) {
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (addr && dladdr((void*)addr, &info) && info.dli_fname) {
        uintptr_t base = (uintptr_t)info.dli_fbase;
        if (info.dli_sname && info.dli_saddr) {
            println("gladio-crash: %s %p %s+0x%lx (%s+0x%lx)", label, (void*)addr, baseName(info.dli_fname),
                    (unsigned long)(addr - base), info.dli_sname, (unsigned long)(addr - (uintptr_t)info.dli_saddr));
        }
        else println("gladio-crash: %s %p %s+0x%lx", label, (void*)addr, baseName(info.dli_fname), (unsigned long)(addr - base));
    }
    else println("gladio-crash: %s %p", label, (void*)addr);
}

static const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        default: return "?";
    }
}

static void scanStack(uintptr_t sp) {
    uintptr_t stackEnd = sp + 8192;
    pthread_attr_t attr;
    /* der Hauptthread läse dafür /proc/self/maps (Speicheranforderung im Handler): dort fest 8 KB */
    if (ownTid() != getpid() && pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* stackAddr = NULL;
        size_t stackSize = 0;
        if (pthread_attr_getstack(&attr, &stackAddr, &stackSize) == 0 && stackAddr) {
            uintptr_t top = (uintptr_t)stackAddr + stackSize;
            if (top > sp && top < stackEnd) stackEnd = top;
        }
        pthread_attr_destroy(&attr);
    }

    int found = 0;
    for (uintptr_t p = sp & ~(uintptr_t)(sizeof(uintptr_t) - 1); p + sizeof(uintptr_t) <= stackEnd && found < 28; p += sizeof(uintptr_t)) {
        uintptr_t value = *(uintptr_t*)p;
        if (!isCodeAddress(value)) continue;
        char label[48];
        snprintf(label, sizeof(label), "Stack sp+0x%lx:", (unsigned long)(p - sp));
        printAddress(label, value);
        found++;
    }
    if (found == 0) println("gladio-crash: keine Rücksprungadressen auf dem Stack gefunden");
}

static void printRegisters(void* ucontextPtr, uintptr_t* outSp) {
    ucontext_t* uc = ucontextPtr;
    *outSp = 0;
    if (!uc) return;
#if defined(__arm__)
    const struct sigcontext* mc = &uc->uc_mcontext;
    println("gladio-crash: r0 %08lx r1 %08lx r2 %08lx r3 %08lx r4 %08lx r5 %08lx r6 %08lx r7 %08lx",
            mc->arm_r0, mc->arm_r1, mc->arm_r2, mc->arm_r3, mc->arm_r4, mc->arm_r5, mc->arm_r6, mc->arm_r7);
    println("gladio-crash: r8 %08lx r9 %08lx r10 %08lx fp %08lx ip %08lx sp %08lx lr %08lx pc %08lx cpsr %08lx",
            mc->arm_r8, mc->arm_r9, mc->arm_r10, mc->arm_fp, mc->arm_ip, mc->arm_sp, mc->arm_lr, mc->arm_pc, mc->arm_cpsr);
    printAddress("PC", mc->arm_pc);
    printAddress("LR", mc->arm_lr);
    *outSp = mc->arm_sp;
#elif defined(__aarch64__)
    const mcontext_t* mc = &uc->uc_mcontext;
    println("gladio-crash: x0 %016llx x1 %016llx x2 %016llx x3 %016llx",
            (unsigned long long)mc->regs[0], (unsigned long long)mc->regs[1], (unsigned long long)mc->regs[2], (unsigned long long)mc->regs[3]);
    println("gladio-crash: sp %016llx lr %016llx pc %016llx",
            (unsigned long long)mc->sp, (unsigned long long)mc->regs[30], (unsigned long long)mc->pc);
    printAddress("PC", mc->pc);
    printAddress("LR", mc->regs[30]);
    *outSp = mc->sp;
#elif defined(__x86_64__)
    const greg_t* r = uc->uc_mcontext.gregs;
    println("gladio-crash: rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx rsp %llx rip %llx",
            (unsigned long long)r[REG_RAX], (unsigned long long)r[REG_RBX], (unsigned long long)r[REG_RCX], (unsigned long long)r[REG_RDX],
            (unsigned long long)r[REG_RSI], (unsigned long long)r[REG_RDI], (unsigned long long)r[REG_RSP], (unsigned long long)r[REG_RIP]);
    printAddress("PC", r[REG_RIP]);
    *outSp = r[REG_RSP];
#elif defined(__i386__)
    const greg_t* r = uc->uc_mcontext.gregs;
    println("gladio-crash: eax %x ebx %x ecx %x edx %x esi %x edi %x esp %x eip %x",
            r[REG_EAX], r[REG_EBX], r[REG_ECX], r[REG_EDX], r[REG_ESI], r[REG_EDI], r[REG_ESP], r[REG_EIP]);
    printAddress("PC", r[REG_EIP]);
    *outSp = r[REG_ESP];
#endif
}

static void crashHandler(int sig, siginfo_t* info, void* ucontextPtr) {
    int index = 0;
    while (index < CRASH_SIGNAL_COUNT && crashSignals[index] != sig) index++;

    if (!__sync_bool_compare_and_swap(&handling, 0, 1)) {
        /* Absturz im Bericht selbst oder in einem zweiten Thread: gleich weiter an den alten Handler */
        if (index < CRASH_SIGNAL_COUNT) sigaction(sig, &oldActions[index], NULL);
        return;
    }

    char threadName[17] = {0};
    prctl(PR_GET_NAME, threadName, 0, 0, 0);
    int slot = findSlot(ownTid(), false);
    const char* phase = slot >= 0 ? trackedThreads[slot].phase : NULL;
    println("gladio-crash: ======== %s (Code %d) an Adresse %p, Thread %ld (%s)%s%s ========",
            signalName(sig), info ? info->si_code : 0, info ? info->si_addr : NULL, (long)ownTid(), threadName,
            phase ? ", in " : "", phase ? phase : "");

    uintptr_t sp = 0;
    printRegisters(ucontextPtr, &sp);
    if (sp) scanStack(sp);

    GLContext* context = slot >= 0 ? trackedThreads[slot].context : NULL;
    if (context) {
        GLRenderer* renderer = currentRenderer;
        println("gladio-crash: Verbindung fd %d, GLX-Kontext %d, %u Anfragen", context->clientFd,
                renderer ? renderer->contextId : -1, context->lastRequests.count);
        int size = context->inputBuffer.size;
        if (context->inputBuffer.buffer && size > 0) {
            const int* args = (const int*)context->inputBuffer.buffer;
            int n = size / 4 < 12 ? size / 4 : 12;
            char line[160];
            int pos = 0;
            for (int i = 0; i < n && pos < (int)sizeof(line) - 12; i++) pos += snprintf(line + pos, sizeof(line) - pos, " %x", args[i]);
            println("gladio-crash: laufende Anfrage: %d Byte, Argumente:%s", size, pos ? line : " -");
        }
        gladio_printLastRequests(context, "gladio-crash: letzte Anfragen");
    }
    println("gladio-crash: ======== Ende des Berichts ========");

    /* alten Handler wieder einsetzen: der Fehler tritt beim Rücksprung erneut auf und geht an ihn
     * (ART/debuggerd); bei abort() & Co. das Signal neu auslösen */
    if (index < CRASH_SIGNAL_COUNT) sigaction(sig, &oldActions[index], NULL);
    if (info && info->si_code <= 0) syscall(SYS_tgkill, getpid(), ownTid(), sig);
}

static void installHandlers(void) {
    refreshCodeRanges();
    for (int i = 0; i < CRASH_SIGNAL_COUNT; i++) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = crashHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(crashSignals[i], &sa, &oldActions[i]);
    }
}

void GladioCrash_install(void) {
    pthread_once(&installOnce, installHandlers);
}

void GladioCrash_setThreadContext(GLContext* context) {
    int slot = findSlot(ownTid(), context != NULL);
    if (slot < 0) return;
    trackedThreads[slot].context = context;
    releaseSlotIfUnused(slot);
    /* inzwischen geladene Bibliotheken (Treiber) mit aufnehmen */
    if (context) refreshCodeRanges();
}

void GladioCrash_setPhase(const char* phase) {
    int slot = findSlot(ownTid(), phase != NULL);
    if (slot < 0) return;
    trackedThreads[slot].phase = phase;
    releaseSlotIfUnused(slot);
}
