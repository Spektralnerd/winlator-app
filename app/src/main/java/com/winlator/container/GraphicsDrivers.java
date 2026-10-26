package com.winlator.container;

import android.content.Context;

import com.winlator.core.Arch;
import com.winlator.core.GPUHelper;
import com.winlator.core.KeyValueSet;

import java.io.IOException;

public abstract class GraphicsDrivers {
    public static final String TURNIP = "turnip";
    public static final String VORTEK = "vortek";
    public static final String ZINK = "zink";
    public static final String VIRGL = "virgl";
    public static final String GLADIO = "gladio";
    /**
     * Software-Rendering auf der CPU (Mesa llvmpipe/softpipe). Im 32-Bit-Build steckt es in dessen
     * VirGL-Bibliothek; die Builds mit der originalen 64-Bit-Root-FS (arm64, armv7hybrid) haben dafür
     * ein eigenes Paket ({@link #SOFTWARE_PACKAGE_64}, gebaut von armv7/scripts/mesa-llvmpipe-arm64.sh).
     * Fehlt es (Build ohne CI), gibt es den Treiber dort nicht.
     */
    public static final String SOFTWARE = "software";
    /** Paket des Software-Treibers für die 64-Bit-Root-FS, ohne Endung (.tzst; .id mit der Prüfsumme) */
    public static final String SOFTWARE_PACKAGE_64 = "graphics_driver/llvmpipe-23.1.9";
    public static final String DEFAULT_VULKAN_DRIVER = VORTEK;
    public static final String DEFAULT_OPENGL_DRIVER = GLADIO;
    private static Boolean softwareAvailable;

    /** Gibt es in dieser APK den Software-Treiber? */
    public static synchronized boolean isSoftwareAvailable(Context context) {
        if (Arch.X86_ONLY) return true;
        if (softwareAvailable == null) {
            try {
                context.getAssets().open(SOFTWARE_PACKAGE_64+".tzst").close();
                softwareAvailable = true;
            }
            catch (IOException e) {
                softwareAvailable = false;
            }
        }
        return softwareAvailable;
    }

    public static String getName(String identifier) {
        switch (identifier) {
            case TURNIP: return "Turnip";
            case VORTEK: return "Vortek";
            case ZINK: return "Zink";
            case VIRGL: return "VirGL";
            case GLADIO: return "Gladio";
            case SOFTWARE: return "Software";
            default: return "None";
        }
    }

    public static boolean isVulkanDriver(String identifier) {
        return identifier != null && (identifier.equals(TURNIP) || identifier.equals(VORTEK));
    }

    public static boolean isOpenGLDriver(String identifier) {
        return identifier != null && (identifier.equals(ZINK) || identifier.equals(VIRGL) || identifier.equals(GLADIO) || identifier.equals(SOFTWARE));
    }

    public static String[] getItems(Context context, String apiName) {
        if (apiName.equalsIgnoreCase("VULKAN")) {
            return new String[]{getName(TURNIP), getName(VORTEK)};
        }
        else if (apiName.equalsIgnoreCase("OPENGL")) {
            if (isSoftwareAvailable(context)) return new String[]{getName(ZINK), getName(VIRGL), getName(GLADIO), getName(SOFTWARE)};
            return new String[]{getName(ZINK), getName(VIRGL), getName(GLADIO)};
        }
        else return new String[0];
    }

    public static String[] parseIdentifiers(String graphicsDriver) {
        if (graphicsDriver == null || graphicsDriver.isEmpty()) return new String[]{DEFAULT_VULKAN_DRIVER, DEFAULT_OPENGL_DRIVER};
        if (graphicsDriver.contains(",")) {
            return graphicsDriver.split(",");
        }
        else {
            if (isVulkanDriver(graphicsDriver)) {
                return new String[]{graphicsDriver, DEFAULT_OPENGL_DRIVER};
            }
            else if (isOpenGLDriver(graphicsDriver)) {
                return new String[]{DEFAULT_VULKAN_DRIVER, graphicsDriver};
            }
            else return new String[]{DEFAULT_VULKAN_DRIVER, DEFAULT_OPENGL_DRIVER};
        }
    }

    public static KeyValueSet[] parseConfigs(String graphicsDriver, String graphicsDriverConfig) {
        if (graphicsDriverConfig == null || graphicsDriverConfig.isEmpty()) return new KeyValueSet[]{new KeyValueSet(), new KeyValueSet()};
        int separator = graphicsDriverConfig.indexOf("|");
        if (separator != -1) {
            String first = graphicsDriverConfig.substring(0, separator);
            String second = graphicsDriverConfig.substring(separator + 1);
            return new KeyValueSet[]{new KeyValueSet(first), new KeyValueSet(second)};
        }
        else {
            if (isVulkanDriver(graphicsDriver)) {
                return new KeyValueSet[]{new KeyValueSet(graphicsDriverConfig), new KeyValueSet()};
            }
            else if (isOpenGLDriver(graphicsDriver)) {
                return new KeyValueSet[]{new KeyValueSet(), new KeyValueSet(graphicsDriverConfig)};
            }
            else return new KeyValueSet[]{new KeyValueSet(), new KeyValueSet()};
        }
    }

    public static String getDefaultDriver(Context context) {
        // Turnip gibt es für 32 Bit nur als experimentellen Eigenbau, daher dort Vortek als Vorgabe.
        // Gladio (wie VirGL) braucht OpenGL ES 3 auf dem Gerät, sonst bleibt Software-Rendering.
        if (Arch.X86_ONLY) return VORTEK+","+(GPUHelper.getGLESMajorVersion(context) >= 3 ? DEFAULT_OPENGL_DRIVER : SOFTWARE);
        return GPUHelper.getAdrenoModelId(context) > 0 ? GraphicsDrivers.TURNIP+","+GraphicsDrivers.DEFAULT_OPENGL_DRIVER : GraphicsDrivers.VORTEK+","+GraphicsDrivers.DEFAULT_OPENGL_DRIVER;
    }
}
