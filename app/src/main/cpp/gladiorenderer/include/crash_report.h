/*
 * Absturzbericht des Gladio-Servers (armv7-Variante).
 *
 * Auf älteren Kerneln (z. B. 3.0 auf dem Galaxy Note 8.0) liefert Androids crash_dump keinen
 * Stack ("Check failed: 0 == stop_signal"). Deshalb schreibt der Server bei SIGSEGV/SIGBUS/
 * SIGILL/SIGFPE/SIGABRT selbst ins Protokoll (Tag "System.out", Zeilen "gladio-crash: ..."):
 * Signal, Fehleradresse, Register, Bibliothek+Versatz von PC/LR (mit nächstem exportiertem
 * Symbol), Rücksprungadressen auf dem Stack und – im Thread einer Client-Verbindung – die
 * letzten Anfragen samt den ersten Argumenten der laufenden. Danach übernimmt der vorherige
 * Signal-Handler (ART/debuggerd) wie gewohnt.
 */
#ifndef GLADIO_CRASH_REPORT_H
#define GLADIO_CRASH_REPORT_H

struct GLContext;

/* einmal je Prozess (mehrfacher Aufruf schadet nicht) */
extern void GladioCrash_install(void);
/* Verbindung, deren Anfragen dieser Thread abarbeitet (NULL: keine) */
extern void GladioCrash_setThreadContext(struct GLContext* context);
/* was dieser Thread gerade tut, z. B. "createGLXContext" (NULL: nichts Besonderes) */
extern void GladioCrash_setPhase(const char* phase);

#endif
