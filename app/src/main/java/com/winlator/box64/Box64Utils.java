package com.winlator.box64;

import android.content.Context;

import androidx.preference.PreferenceManager;

import com.winlator.core.Arch;
import com.winlator.core.ArrayUtils;
import com.winlator.core.DefaultVersion;
import com.winlator.core.StreamUtils;
import com.winlator.xenvironment.RootFS;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;

public abstract class Box64Utils {
    public static String extractBinVersion(Context context) {
        if (Arch.X86_ONLY) {
            // Box86 setzt seine Versionsangabe erst zur Laufzeit zusammen (printf), im Binary steht sie nicht
            return PreferenceManager.getDefaultSharedPreferences(context).getString("current_box64_version", DefaultVersion.BOX64);
        }
        File binFile = new File(RootFS.find(context).getRootDir(), "/usr/local/bin/"+Arch.BOX);
        try (BufferedInputStream inStream = new BufferedInputStream(new FileInputStream(binFile), StreamUtils.BUFFER_SIZE)) {
            int bytesRead;
            byte[] buffer = new byte[4096];
            // Box64: "Box64 arm64 v0.4.4 …", Box86: "Box86 with Dynarec v0.3.8 …"
            final byte[] str = (Arch.X86_ONLY ? "Box86 with Dynarec v" : "Box64 arm64 v").getBytes();
            while ((bytesRead = inStream.read(buffer)) != -1) {
                int index = ArrayUtils.indexOf(buffer, 0, bytesRead, str);
                if (index != ArrayUtils.INDEX_NOT_FOUND) {
                    int start = index + str.length;
                    int end = ArrayUtils.indexOf(buffer, start, bytesRead, (byte)' ');
                    return end != ArrayUtils.INDEX_NOT_FOUND ? new String(buffer, start, end - start) : "";
                }
            }
        }
        catch (IOException e) {}
        return "";
    }
}
