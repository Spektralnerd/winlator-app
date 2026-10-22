package com.winlator.core;

import com.winlator.BuildConfig;

import java.io.File;

/**
 * Bauvariante dieser APK (siehe app/build.gradle, -PwinlatorArch=...).
 *
 *  arm64        Original: Box64 + WoW64-Wine (system32 = 64 Bit, syswow64 = 32 Bit)
 *  armv7hybrid  32-Bit-App, aber der originale 64-Bit-Unterbau (braucht einen 64-Bit-Kernel)
 *  armv7        alles 32 Bit: Box86 + i386-Wine, Win32-Präfix (system32 = 32 Bit, kein syswow64)
 */
public final class Arch {
    public static final String WINLATOR_ARCH = BuildConfig.WINLATOR_ARCH;
    /** true: Gast ist reines x86 (Box86, i386-Wine, Win32-Präfix) */
    public static final boolean X86_ONLY = BuildConfig.X86_ONLY;
    /** true: Die App selbst läuft als 64-Bit-Prozess */
    public static final boolean APP_IS_64BIT = WINLATOR_ARCH.equals("arm64");

    public static final String BOX = X86_ONLY ? "box86" : "box64";
    public static final String BOX_TITLE = X86_ONLY ? "Box86" : "Box64";
    public static final String BOX_ENV_PREFIX = X86_ONLY ? "BOX86_" : "BOX64_";
    /** x86-Bibliotheken, die Box86/Box64 emulieren statt durchzureichen */
    public static final String BOX_LD_LIBRARY_DIR = X86_ONLY ? "/lib/i386-linux-gnu" : "/lib/x86_64-linux-gnu";
    /** Android-Systembibliotheken passend zur Bitbreite der App */
    public static final String SYSTEM_LIB_DIR = APP_IS_64BIT ? "/system/lib64" : "/system/lib";

    /** Wine-Ordner, dessen DLLs nach C:\windows\system32 gehören */
    public static final String WINE_SYSTEM32_ARCH = X86_ONLY ? "i386-windows" : "x86_64-windows";

    private Arch() {}

    /** Wandelt einen BOX64_*-Namen in den passenden Namen für diese Variante um. */
    public static String boxEnv(String box64Name) {
        if (X86_ONLY && box64Name.startsWith("BOX64_")) return "BOX86_"+box64Name.substring(6);
        return box64Name;
    }

    /**
     * Box64-Optionen, die Box86 nicht kennt. Box86 würde sie zwar ignorieren, sie sollen aber
     * nicht als wirksam erscheinen.
     */
    public static boolean isBoxEnvSupported(String box64Name) {
        if (!X86_ONLY) return true;
        switch (box64Name) {
            case "BOX64_DYNAREC_NATIVEFLAGS":
            case "BOX64_DYNAREC_WEAKBARRIER":
            case "BOX64_DYNAREC_DIRTY":
            case "BOX64_DYNACACHE":
            case "BOX64_UNITYPLAYER":
                return false;
            default:
                return true;
        }
    }

    /**
     * Die mitgelieferten Komponenten (DXVK, VKD3D, WineD3D, D8VK, cnc-ddraw, Windows-Komponenten …)
     * sind für ein WoW64-Präfix gepackt: system32/ = 64 Bit, syswow64/ = 32 Bit.
     * Im Win32-Präfix gehören die 32-Bit-Dateien nach system32, die 64-Bit-Dateien entfallen.
     */
    public static TarCompressorUtils.OnExtractFileListener win32Remap(final TarCompressorUtils.OnExtractFileListener listener) {
        if (!X86_ONLY) return listener;
        return (destination, size) -> {
            File remapped = remapToWin32(destination);
            if (remapped == null) return null;
            return listener != null ? listener.onExtractFile(remapped, size) : remapped;
        };
    }

    public static File remapToWin32(File destination) {
        if (!X86_ONLY) return destination;
        String path = destination.getPath();
        if (path.contains("/system32/") || path.endsWith("/system32")) return null;
        int index = path.indexOf("/syswow64");
        if (index != -1) return new File(path.substring(0, index)+"/system32"+path.substring(index+9));
        return destination;
    }

    /** Ordner im Präfix für 32-Bit-DLLs: syswow64 (WoW64) bzw. system32 (Win32) */
    public static String sys32DirName() {
        return X86_ONLY ? "system32" : "syswow64";
    }
}
