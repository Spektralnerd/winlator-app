package com.winlator.core;

import android.content.Context;
import android.os.Build;
import android.os.Environment;

import com.winlator.BuildConfig;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.Date;
import java.util.Locale;

/**
 * Diagnose für die 32-Bit-Builds, ohne adb und ohne vorher etwas einzuschalten:
 * <ul>
 *   <li>Documents/Winlator/start.txt wird bei jedem Containerstart neu geschrieben: Gerät,
 *       Container-Einstellungen, OpenGL-ES-Kontext, Befehl des Gastprogramms, dessen Ausgabe
 *       (Anfang und Ende) und sein Endstatus</li>
 *   <li>Documents/Winlator/crash.txt sammelt nicht abgefangene Java-Ausnahmen der App</li>
 * </ul>
 * Jede Zeile wird sofort geschrieben, damit nach einem Absturz sichtbar ist, wie weit der Start kam.
 * Im Original (arm64) ist alles abgeschaltet.
 */
public final class StartLog {
    public static final boolean ENABLED = !Arch.APP_IS_64BIT;
    private static final int HEAD_LINES = 400;
    private static final int TAIL_LINES = 150;
    private static final ArrayDeque<String> tail = new ArrayDeque<>();
    private static File startFile;
    private static File crashFile;
    private static int guestLines = 0;
    private static boolean crashHandlerInstalled = false;

    /** Ausgabe des Gastprogramms (als Debug-Callback von ProcessHelper) */
    public static final Callback<String> GUEST_OUTPUT = StartLog::guestLine;

    private StartLog() {}

    private static File getDir(Context context) {
        File dir = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS), "Winlator");
        if (dir.isDirectory() || dir.mkdirs()) return dir;
        File fallback = context.getExternalFilesDir(null);
        return fallback != null ? fallback : context.getFilesDir();
    }

    private static String deviceInfo() {
        return "Winlator " + BuildConfig.VERSION_NAME + " (" + BuildConfig.WINLATOR_ARCH + "), Android " +
            Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + "), " + Build.MANUFACTURER + " " +
            Build.MODEL + ", " + Build.HARDWARE + ", ABIs " + String.join(",", Build.SUPPORTED_ABIS);
    }

    private static String now() {
        return new SimpleDateFormat("HH:mm:ss.SSS", Locale.ROOT).format(new Date());
    }

    private static void write(File file, String text, boolean append) {
        if (file == null) return;
        try (FileWriter writer = new FileWriter(file, append)) {
            writer.write(text);
        }
        catch (IOException e) {}
    }

    /** Nicht abgefangene Ausnahmen zusätzlich in crash.txt (und ins laufende Startprotokoll) schreiben */
    public static synchronized void installCrashHandler(Context context) {
        if (!ENABLED || crashHandlerInstalled) return;
        crashHandlerInstalled = true;
        crashFile = new File(getDir(context.getApplicationContext()), "crash.txt");
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, throwable) -> {
            try {
                StringWriter stackTrace = new StringWriter();
                throwable.printStackTrace(new PrintWriter(stackTrace));
                String text = "==== " + new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new Date()) +
                    "\n" + deviceInfo() + "\nThread: " + thread.getName() + "\n" + stackTrace + "\n";
                write(crashFile, text, true);
                add("Absturz der App (Details in crash.txt): " + throwable);
            }
            catch (Throwable ignored) {}
            if (previous != null) previous.uncaughtException(thread, throwable);
        });
    }

    /** Neues Startprotokoll beginnen (beim Start eines Containers) */
    public static synchronized void begin(Context context, String title) {
        if (!ENABLED) return;
        startFile = new File(getDir(context), "start.txt");
        guestLines = 0;
        tail.clear();
        write(startFile, now() + " " + title + "\n" + now() + " " + deviceInfo() + "\n", false);
    }

    public static synchronized void add(String line) {
        if (!ENABLED || startFile == null) return;
        write(startFile, now() + " " + line + "\n", true);
    }

    private static synchronized void guestLine(String line) {
        if (startFile == null) return;
        if (guestLines < HEAD_LINES) {
            write(startFile, "| " + line + "\n", true);
        }
        else {
            tail.addLast(line);
            if (tail.size() > TAIL_LINES) tail.removeFirst();
        }
        guestLines++;
    }

    /** Endstatus des Gastprogramms, dazu die letzten zurückgehaltenen Ausgabezeilen */
    public static synchronized void guestExited(int status) {
        if (!ENABLED || startFile == null) return;
        if (!tail.isEmpty()) {
            int skipped = guestLines - HEAD_LINES - tail.size();
            StringBuilder sb = new StringBuilder();
            if (skipped > 0) sb.append("| … ").append(skipped).append(" Zeilen ausgelassen …\n");
            for (String line : tail) sb.append("| ").append(line).append('\n');
            write(startFile, sb.toString(), true);
            tail.clear();
        }
        add("Gastprogramm beendet, Status " + status);
    }
}
