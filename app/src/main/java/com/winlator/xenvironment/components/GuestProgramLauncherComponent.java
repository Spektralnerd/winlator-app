package com.winlator.xenvironment.components;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Process;

import androidx.preference.PreferenceManager;

import com.winlator.box64.Box64Preset;
import com.winlator.box64.Box64PresetManager;
import com.winlator.core.StartLog;
import com.winlator.core.Arch;
import com.winlator.core.Callback;
import com.winlator.core.DefaultVersion;
import com.winlator.core.EnvVars;
import com.winlator.core.FileUtils;
import com.winlator.core.GeneralComponents;
import com.winlator.core.LocaleHelper;
import com.winlator.core.ProcessHelper;
import com.winlator.widget.LogView;
import com.winlator.xconnector.UnixSocketConfig;
import com.winlator.xenvironment.EnvironmentComponent;
import com.winlator.xenvironment.RootFS;

import java.io.File;
import java.util.List;

public class GuestProgramLauncherComponent extends EnvironmentComponent {
    private String guestExecutable;
    private static int pid = -1;
    private EnvVars envVars;
    private String box64Preset = Box64Preset.CONSERVATIVE;
    private Callback<Integer> terminationCallback;
    private static final Object lock = new Object();

    @Override
    public void start() {
        synchronized (lock) {
            stop();
            extractBox64File();
            copyDefaultBox64RCFile();
            pid = execGuestProgram();
        }
    }

    @Override
    public void stop() {
        synchronized (lock) {
            if (pid != -1) {
                Process.killProcess(pid);
                pid = -1;
            }
        }
    }

    public Callback<Integer> getTerminationCallback() {
        return terminationCallback;
    }

    public void setTerminationCallback(Callback<Integer> terminationCallback) {
        this.terminationCallback = terminationCallback;
    }

    public String getGuestExecutable() {
        return guestExecutable;
    }

    public void setGuestExecutable(String guestExecutable) {
        this.guestExecutable = guestExecutable;
    }

    public EnvVars getEnvVars() {
        return envVars;
    }

    public void setEnvVars(EnvVars envVars) {
        this.envVars = envVars;
    }

    public String getBox64Preset() {
        return box64Preset;
    }

    public void setBox64Preset(String box64Preset) {
        this.box64Preset = box64Preset;
    }

    private int execGuestProgram() {
        RootFS rootFS = environment.getRootFS();
        File rootDir = rootFS.getRootDir();

        EnvVars envVars = new EnvVars();
        addBox64EnvVars(envVars);
        LocaleHelper.setEnvVars(envVars);

        envVars.put("HOME", rootDir+RootFS.HOME_PATH);
        envVars.put("USER", RootFS.USER);
        envVars.put("TMPDIR", rootDir+"/tmp");
        envVars.put("DISPLAY", ":0");
        envVars.put("PATH", rootDir+rootFS.getWinePath()+"/bin:"+rootDir+"/usr/local/bin:"+rootDir+"/usr/bin");
        envVars.put("LD_LIBRARY_PATH", rootFS.getLibDir().getPath());
        envVars.put(Arch.BOX_ENV_PREFIX+"LD_LIBRARY_PATH", rootDir+Arch.BOX_LD_LIBRARY_DIR);
        envVars.put("ANDROID_SYSVSHM_SERVER", rootDir+UnixSocketConfig.SYSVSHM_SERVER_PATH);
        addRootFSEnvVars(envVars, rootDir);

        if (this.envVars != null) envVars.putAll(this.envVars);

        File shmDir = new File(rootDir, "/tmp/shm");
        if (!shmDir.isDirectory()) shmDir.mkdirs();

        String command = rootDir+"/usr/local/bin/"+Arch.BOX+" "+guestExecutable;

        if (StartLog.ENABLED) {
            StartLog.add("Gastprogramm: "+command);
            StartLog.add("Umgebung: "+envVars);
            ProcessHelper.addDebugCallback(StartLog.GUEST_OUTPUT);
        }

        int newPid = ProcessHelper.exec(command, envVars, rootDir, (status) -> {
            synchronized (lock) {
                pid = -1;
            }
            StartLog.guestExited(status);
            if (terminationCallback != null) terminationCallback.call(status);
        });
        StartLog.add(newPid > 0 ? "Gastprogramm gestartet, PID "+newPid : "Gastprogramm konnte nicht gestartet werden");
        return newPid;
    }

    private void extractBox64File() {
        Context context = environment.getContext();
        SharedPreferences preferences = PreferenceManager.getDefaultSharedPreferences(context);
        String box64Version = preferences.getString("box64_version", DefaultVersion.BOX64);
        String currentBox64Version = preferences.getString("current_box64_version", "");

        if (!box64Version.equals(currentBox64Version)) {
            GeneralComponents.extractFile(GeneralComponents.Type.BOX64, context, box64Version, DefaultVersion.BOX64);
            preferences.edit().putString("current_box64_version", box64Version).apply();
        }
    }

    private void copyDefaultBox64RCFile() {
        Context context = environment.getContext();
        RootFS rootFS = environment.getRootFS();
        FileUtils.copy(context, Arch.BOX+"/default."+Arch.BOX+"rc", new File(rootFS.getRootDir(), "/etc/config."+Arch.BOX+"rc"));
    }

    private void addBox64EnvVars(EnvVars envVars) {
        Context context = environment.getContext();
        RootFS rootFS = environment.getRootFS();
        SharedPreferences preferences = PreferenceManager.getDefaultSharedPreferences(context);
        int box64Logs = preferences.getInt("box64_logs", 0);
        boolean saveToFile = preferences.getBoolean("save_logs_to_file", false);

        final String p = Arch.BOX_ENV_PREFIX;
        envVars.put(p+"NOBANNER", box64Logs >= 1 ? "0" : "1");
        envVars.put(p+"DYNAREC", "1");
        if (!Arch.X86_ONLY) {
            envVars.put("BOX64_UNITYPLAYER", "0");
            envVars.put("BOX64_DYNACACHE", "0");
        }

        if (box64Logs >= 1) {
            envVars.put(p+"LOG", "1");
            envVars.put(p+"DYNAREC_MISSING", "1");

            if (box64Logs == 2) {
                envVars.put(p+"SHOWSEGV", "1");
                envVars.put(p+"DLSYM_ERROR", "1");
                envVars.put(p+"TRACE_FILE", "stderr");

                if (saveToFile) {
                    File parent = (new File(preferences.getString("log_file", LogView.getLogFile().getPath()))).getParentFile();
                    if (parent != null && parent.isDirectory()) {
                        File traceDir = new File(parent, "trace");
                        if (!traceDir.isDirectory()) traceDir.mkdirs();
                        FileUtils.clear(traceDir);

                        envVars.put(p+"TRACE_FILE", traceDir+"/"+Arch.BOX+"-%pid.txt");
                    }
                }
            }
        }

        // 32 Bit: Box86 schreibt sein Protokoll sonst nach stdout, und das schließt Wine bei
        // Programmen ohne Konsole (z. B. aus dem Dateimanager gestartet); stderr bleibt erhalten
        if (Arch.X86_ONLY && box64Logs >= 1 && !envVars.has(p+"TRACE_FILE")) envVars.put(p+"TRACE_FILE", "stderr");

        envVars.putAll(Box64PresetManager.getEnvVars(context, box64Preset));

        File box64RCFile = new File(rootFS.getRootDir(), "/etc/config."+Arch.BOX+"rc");
        envVars.put(p+"RCFILE", box64RCFile.getPath());
    }

    /**
     * Zusätzliche Umgebungsvariablen, die das Root-FS selbst mitbringt (rootfs/.winlator/env,
     * Zeilen "NAME=Wert", "@ROOT@" wird durch den Root-FS-Pfad ersetzt). Das 32-Bit-Root-FS nutzt
     * das für Pfade von Bibliotheken, die nicht für den Winlator-Präfix gebaut sind.
     */
    private static void addRootFSEnvVars(EnvVars envVars, File rootDir) {
        File envFile = new File(rootDir, "/.winlator/env");
        if (!envFile.isFile()) return;
        for (String line : FileUtils.readLines(envFile)) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            int index = line.indexOf('=');
            if (index <= 0) continue;
            envVars.put(line.substring(0, index).trim(), line.substring(index+1).trim().replace("@ROOT@", rootDir.getPath()));
        }
    }

    @Override
    public void onPause() {
        synchronized (lock) {
            if (pid != -1) {
                List<ProcessHelper.PStat> processes = ProcessHelper.getChildProcesses();
                for (int i = processes.size()-1; i >= 0; i--) {
                    ProcessHelper.PStat process = processes.get(i);
                    if (process.guestProcess && process.state != ProcessHelper.PState.STOPPED) {
                        ProcessHelper.suspendProcess(process.pid);
                    }
                }
            }
        }
    }

    @Override
    public void onResume() {
        synchronized (lock) {
            if (pid != -1) {
                List<ProcessHelper.PStat> processes = ProcessHelper.getChildProcesses();
                for (int i = 0; i < processes.size(); i++) {
                    ProcessHelper.PStat process = processes.get(i);
                    if (process.guestProcess && process.state == ProcessHelper.PState.STOPPED) {
                        ProcessHelper.resumeProcess(process.pid);
                    }
                }
            }
        }
    }
}