package com.winlator.core;

import com.winlator.container.GraphicsDrivers;

import java.util.Locale;

public abstract class DefaultVersion {
    // In der 32-Bit-Variante (Arch.X86_ONLY) steht BOX64 für die Box86-Version
    public static final String BOX64 = Arch.X86_ONLY ? "0.3.9" : "0.4.4";
    public static final String TURNIP = Arch.X86_ONLY ? "26.1.0" : "26.2.0";
    public static final String VORTEK = "2.1";
    public static final String ZINK = "22.2.5";
    public static final String VIRGL = "23.1.9";
    public static final String GLADIO = "1.1";
    public static final String D7VK = "1.11";
    public static final String D8VK = "1.0";
    public static final String VKD3D = "2.14.1";
    public static final String WINED3D = WineInfo.MAIN_WINE_VERSION;
    public static final String CNC_DDRAW = "6.6";
    public static final String SOUNDFONT = "GeneralUser-GS";  // GeneralUser GS 2.0.3 (S. Christian Collins), GS-kompatibel; vorher SONiVOX EAS
    public static final String SOUNDFONT_SMALL = "SONiVOX-EAS-GM-Wavetable";
    public static final String MINOR_DXVK = "1.10.3";
    public static final String MAJOR_DXVK = "2.4.1";

    public static String DXVK() {
        return DXVK(null);
    }

    public static String DXVK(String vulkanDriver) {
        int vkApiVersion = 0;
        if (vulkanDriver != null && vulkanDriver.equals(GraphicsDrivers.VORTEK)) vkApiVersion = GPUHelper.vkGetApiVersion();
        return vulkanDriver == null || vulkanDriver.equals(GraphicsDrivers.TURNIP) || vkApiVersion >= GPUHelper.vkMakeVersion(1, 3, 0) ? MAJOR_DXVK : MINOR_DXVK;
    }

    public static String valueOf(String name) {
        switch (name.toUpperCase(Locale.ENGLISH)) {
            case "BOX64": return BOX64;
            case "TURNIP": return TURNIP;
            case "VORTEK": return VORTEK;
            case "ZINK": return ZINK;
            case "VIRGL": return VIRGL;
            case "GLADIO": return GLADIO;
            case "D7VK": return D7VK;
            case "D8VK": return D8VK;
            case "VKD3D": return VKD3D;
            case "WINED3D": return WINED3D;
            case "CNC_DDRAW": return CNC_DDRAW;
            case "SOUNDFONT": return SOUNDFONT;
            default: return "0.0";
        }
    }
}