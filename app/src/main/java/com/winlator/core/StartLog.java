package com.winlator.core;

import android.app.ActivityManager;
import android.content.Context;
import android.content.pm.ConfigurationInfo;
import android.os.Build;
import android.os.Environment;
import android.os.SystemClock;

import com.winlator.BuildConfig;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.io.Writer;
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
 *   <li>Documents/Winlator/logcat.txt: das Android-Protokoll der App (nur ihre eigenen Einträge),
 *       gesichert, wenn das Gastprogramm endet, und beim nächsten App-Start, wenn die App während
 *       eines Containers abgestürzt ist (z. B. ein Absturz im nativen Code, den crash.txt nicht sieht)</li>
 * </ul>
 * Jede Zeile wird sofort geschrieben, damit nach einem Absturz sichtbar ist, wie weit der Start kam.
 * Im Original (arm64) ist alles abgeschaltet.
 */
public final class StartLog {
    public static final boolean ENABLED = !Arch.APP_IS_64BIT;
    private static final int HEAD_LINES = 400;
    private static final int TAIL_LINES = 150;
    private static final int LOGCAT_LINES = 5000;
    private static final String MARKER_NAME = "startlog-running";
    private static final ArrayDeque<String> tail = new ArrayDeque<>();
    private static File logDir;
    private static File startFile;
    private static File markerFile;
    private static int guestLines = 0;
    private static boolean guestDone = false;
    private static volatile long lastGuestLineTime = 0;
    private static boolean crashHandlerInstalled = false;

    /** Ausgabe des Gastprogramms (als Debug-Callback von ProcessHelper) */
    public static final Callback<String> GUEST_OUTPUT = StartLog::guestLine;

    private StartLog() {}

    /** Documents/Winlator, solange die Speicherberechtigung fehlt Android/data/com.winlator/files */
    private static synchronized File getDir(Context context) {
        if (logDir != null) return logDir;
        File dir = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS), "Winlator");
        if (dir.isDirectory() || dir.mkdirs()) return logDir = dir;
        File fallback = context.getExternalFilesDir(null);
        return fallback != null ? fallback : context.getFilesDir();
    }

    private static String deviceInfo() {
        return "Winlator " + BuildConfig.VERSION_NAME + " (" + BuildConfig.WINLATOR_ARCH + "), Android " +
            Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + "), " + Build.MANUFACTURER + " " +
            Build.MODEL + ", " + Build.HARDWARE + ", ABIs " + String.join(",", Build.SUPPORTED_ABIS) +
            ", Kernel " + System.getProperty("os.version");
    }

    /** Was Box86 und die Grafik brauchen: CPU-Merkmale, OpenGL ES, Speicher */
    private static String systemInfo(Context context) {
        StringBuilder sb = new StringBuilder();
        String features = null, cpuPart = null, hardware = null;
        int cores = 0;
        try (BufferedReader reader = new BufferedReader(new FileReader("/proc/cpuinfo"))) {
            String line;
            while ((line = reader.readLine()) != null) {
                int colon = line.indexOf(':');
                if (colon < 0) continue;
                String key = line.substring(0, colon).trim(), value = line.substring(colon + 1).trim();
                if (key.equals("processor")) cores++;
                else if (key.equals("Features") && features == null) features = value;
                else if (key.equals("CPU part") && cpuPart == null) cpuPart = value;
                else if (key.equals("Hardware") && hardware == null) hardware = value;
            }
        }
        catch (IOException e) {}
        sb.append("CPU: ").append(cores).append(" Kerne");
        if (cpuPart != null) sb.append(", Part ").append(cpuPart);
        if (hardware != null) sb.append(", ").append(hardware);
        if (features != null) sb.append(", Merkmale ").append(features);

        ActivityManager am = (ActivityManager)context.getSystemService(Context.ACTIVITY_SERVICE);
        if (am != null) {
            ConfigurationInfo info = am.getDeviceConfigurationInfo();
            if (info != null) sb.append("\nOpenGL ES laut System: ").append(info.getGlEsVersion());
            ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
            am.getMemoryInfo(memory);
            sb.append(", Speicher frei ").append(memory.availMem >> 20).append(" von ").append(memory.totalMem >> 20)
              .append(" MB, Grenze für die App ").append(am.getMemoryClass()).append("/").append(am.getLargeMemoryClass()).append(" MB");
        }
        return sb.toString();
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

    /**
     * Nicht abgefangene Ausnahmen zusätzlich in crash.txt (und ins laufende Startprotokoll) schreiben.
     * Ist die App beim letzten Mal während eines Containers verschwunden, ohne dass das Gastprogramm
     * endete (nativer Absturz, vom System beendet), wird jetzt das Android-Protokoll gesichert.
     */
    public static synchronized void installCrashHandler(Context context) {
        if (!ENABLED || crashHandlerInstalled) return;
        crashHandlerInstalled = true;
        final Context appContext = context.getApplicationContext();
        final File dir = getDir(appContext);
        markerFile = new File(appContext.getFilesDir(), MARKER_NAME);
        if (markerFile.isFile()) {
            final String what = FileUtils.readString(markerFile);
            markerFile.delete();
            Thread thread = new Thread(() -> saveLogcat(dir, "App beim letzten Containerstart unerwartet beendet (" +
                (what != null ? what.trim() : "?") + ")"), "StartLog-logcat");
            thread.setDaemon(true);
            thread.start();
        }

        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, throwable) -> {
            try {
                StringWriter stackTrace = new StringWriter();
                throwable.printStackTrace(new PrintWriter(stackTrace));
                String text = "==== " + new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new Date()) +
                    "\n" + deviceInfo() + "\nThread: " + thread.getName() + "\n" + stackTrace + "\n";
                write(new File(getDir(appContext), "crash.txt"), text, true);
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
        guestDone = false;
        tail.clear();
        write(startFile, now() + " " + title + "\n" + now() + " " + deviceInfo() + "\n" + systemInfo(context) + "\n", false);
        markerFile = new File(context.getApplicationContext().getFilesDir(), MARKER_NAME);
        write(markerFile, now() + " " + title, false);
    }

    public static synchronized void add(String line) {
        if (!ENABLED || startFile == null) return;
        write(startFile, now() + " " + line + "\n", true);
    }

    private static synchronized void guestLine(String line) {
        if (startFile == null) return;
        lastGuestLineTime = SystemClock.uptimeMillis();
        if (guestLines < HEAD_LINES || guestDone) {
            write(startFile, "| " + line + "\n", true);
        }
        else {
            tail.addLast(line);
            if (tail.size() > TAIL_LINES) tail.removeFirst();
        }
        guestLines++;
    }

    private static String describeStatus(int status) {
        if (status <= 128 || status > 128 + 64) return String.valueOf(status);
        int signal = status - 128;
        String name;
        switch (signal) {
            case 4: name = "SIGILL, unbekannter Befehl"; break;
            case 6: name = "SIGABRT, abgebrochen"; break;
            case 7: name = "SIGBUS, Speicherzugriff"; break;
            case 8: name = "SIGFPE, Rechenfehler"; break;
            case 9: name = "SIGKILL, beendet"; break;
            case 11: name = "SIGSEGV, Speicherzugriffsfehler"; break;
            case 15: name = "SIGTERM"; break;
            case 31: name = "SIGSYS, Systemaufruf vom Kernel/seccomp verweigert"; break;
            default: name = "Signal " + signal;
        }
        return status + " (" + name + ")";
    }

    /**
     * Endstatus des Gastprogramms, dazu die letzten zurückgehaltenen Ausgabezeilen. Läuft im
     * Wartethread des Prozesses; wartet kurz, bis die Lesethreads den Rest der Ausgabe geliefert
     * haben (gerade die letzten Zeilen sagen, woran es lag), und sichert dann das Android-Protokoll.
     */
    public static void guestExited(int status) {
        if (!ENABLED || startFile == null) return;
        long deadline = SystemClock.uptimeMillis() + 2000;
        try {
            Thread.sleep(200);
            while (SystemClock.uptimeMillis() < deadline && SystemClock.uptimeMillis() - lastGuestLineTime < 300) {
                Thread.sleep(50);
            }
        }
        catch (InterruptedException e) {}

        File dir;
        synchronized (StartLog.class) {
            if (!tail.isEmpty()) {
                int skipped = guestLines - HEAD_LINES - tail.size();
                StringBuilder sb = new StringBuilder();
                if (skipped > 0) sb.append("| … ").append(skipped).append(" Zeilen ausgelassen …\n");
                for (String line : tail) sb.append("| ").append(line).append('\n');
                write(startFile, sb.toString(), true);
                tail.clear();
            }
            guestDone = true;
            add("Gastprogramm beendet, Status " + describeStatus(status));
            dir = startFile.getParentFile();
            if (markerFile != null) markerFile.delete();
        }
        if (dir != null) saveLogcat(dir, "Gastprogramm beendet, Status " + describeStatus(status));
    }

    /**
     * Container wird regulär verlassen (Menü „Beenden“ oder nach dem Ende des Gastprogramms).
     * Beim Verlassen über das Menü wird hier auch das Android-Protokoll gesichert (sonst nur,
     * wenn das Gastprogramm von selbst endet); dauert knapp eine Sekunde.
     */
    public static void end() {
        File dir;
        boolean saveLog;
        synchronized (StartLog.class) {
            if (!ENABLED || startFile == null) return;
            if (markerFile != null) markerFile.delete();
            add("Container verlassen");
            dir = startFile.getParentFile();
            saveLog = !guestDone;
        }
        if (saveLog && dir != null) saveLogcat(dir, "Container verlassen");
    }

    /** Die eigenen Einträge der App aus dem Android-Protokoll nach logcat.txt (ohne adb, ohne Rechte) */
    private static void saveLogcat(File dir, String reason) {
        File file = new File(dir, "logcat.txt");
        java.lang.Process process = null;
        try (Writer writer = new FileWriter(file, false)) {
            writer.write("==== " + new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new Date()) +
                " " + reason + "\n" + deviceInfo() + "\n");
            process = new ProcessBuilder("logcat", "-v", "threadtime", "-b", "main", "-b", "system", "-b", "crash",
                "-t", String.valueOf(LOGCAT_LINES)).redirectErrorStream(true).start();
            process.getOutputStream().close();
            InputStream in = process.getInputStream();
            char[] buffer = new char[8192];
            InputStreamReader reader = new InputStreamReader(in);
            int count;
            while ((count = reader.read(buffer)) != -1) writer.write(buffer, 0, count);
            process.waitFor();
        }
        catch (IOException | InterruptedException e) {
            write(file, "logcat nicht lesbar: " + e + "\n", true);
        }
        finally {
            if (process != null) process.destroy();
        }
    }
}
