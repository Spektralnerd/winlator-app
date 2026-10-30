package com.winlator.renderer;

import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.opengl.GLES30;
import android.opengl.GLES32;

import com.winlator.xserver.Drawable;

import java.nio.ByteBuffer;

public class Texture {
    protected int textureId = 0;
    protected int wrapS = GLES20.GL_CLAMP_TO_EDGE;
    protected int wrapT = GLES20.GL_CLAMP_TO_EDGE;
    protected int magFilter = GLES20.GL_LINEAR;
    protected int minFilter = GLES20.GL_LINEAR;
    protected int format = GLES11Ext.GL_BGRA;
    protected boolean needsUpdate = true;
    private boolean flipY = false;
    /* Textur gehört einem anderen (dem Gladio-Server, ES-2.0-Pfad): nur anzeigen, nie anlegen oder löschen */
    private boolean external = false;
    protected Drawable owner;
    /* copyFromReadBuffer: Größe des zuletzt kopierten Bilds (0: Bild neu anlegen) und die Fence dahinter, auf die das
     * Zeichnen wartet (gesetzt und abgeholt unter drawable.renderLock) */
    private short copiedWidth = 0;
    private short copiedHeight = 0;
    private long copyFence = 0;

    public Texture(Drawable owner) {
        this.owner = owner;
    }

    protected void generateTextureId() {
        int[] textureIds = new int[1];
        GLES20.glGenTextures(1, textureIds, 0);
        textureId = textureIds[0];
    }

    protected void setTextureParameters() {
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, wrapS);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, wrapT);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, magFilter);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, minFilter);
    }

    public void allocateTexture(short width, short height, ByteBuffer data) {
        generateTextureId();
        copiedWidth = copiedHeight = 0;

        GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
        GLES20.glPixelStorei(GLES20.GL_UNPACK_ALIGNMENT, 1);
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId);

        if (data != null) {
            GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, format, width, height, 0, format, GLES20.GL_UNSIGNED_BYTE, data);
        }

        setTextureParameters();
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, 0);
    }

    public Drawable getOwner() {
        return owner;
    }

    public void setOwner(Drawable owner) {
        this.owner = owner;
    }

    public boolean isFlipY() {
        return flipY;
    }

    public void setFlipY(boolean flipY) {
        this.flipY = flipY;
    }

    public int getWrapS() {
        return wrapS;
    }

    public void setWrapS(int wrapS) {
        this.wrapS = wrapS;
    }

    public int getWrapT() {
        return wrapT;
    }

    public void setWrapT(int wrapT) {
        this.wrapT = wrapT;
    }

    public int getMagFilter() {
        return magFilter;
    }

    public void setMagFilter(int magFilter) {
        this.magFilter = magFilter;
    }

    public int getMinFilter() {
        return minFilter;
    }

    public void setMinFilter(int minFilter) {
        this.minFilter = minFilter;
    }

    public int getFormat() {
        return format;
    }

    public void setFormat(int format) {
        this.format = format;
    }

    public boolean isNeedsUpdate() {
        return needsUpdate;
    }

    public void setNeedsUpdate(boolean needsUpdate) {
        this.needsUpdate = needsUpdate;
    }

    public boolean isExternal() {
        return external;
    }

    /* Fremde Textur anzeigen (ES-2.0-Pfad: der Gladio-Server übergibt sein fertiges Bild ohne Kopie) */
    public void setExternalTextureId(int externalTextureId) {
        if (!external) destroy();
        textureId = externalTextureId;
        external = true;
    }

    public void updateFromDrawable() {
        if (owner == null || owner.getData() == null) return;

        ByteBuffer data = owner.getData();
        if (external) {
            textureId = 0;
            external = false;
        }
        if (!isAllocated()) {
            allocateTexture(owner.width, owner.height, data);
        }
        else if (needsUpdate) {
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId);
            GLES20.glTexSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, owner.width, owner.height, format, GLES20.GL_UNSIGNED_BYTE, data);
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, 0);
            needsUpdate = false;
        }
    }

    public boolean isAllocated() {
        return textureId > 0;
    }

    public int getTextureId() {
        return textureId;
    }

    public void copyFromSource(Texture source) {
        if (!source.isAllocated()) source.allocateTexture(source.owner.width, source.owner.height, null);
        if (!this.isAllocated()) this.allocateTexture(source.owner.width, source.owner.height, null);
        GLES32.glCopyImageSubData(
            source.textureId, GLES20.GL_TEXTURE_2D, 0, 0, 0, 0,
            this.textureId, GLES20.GL_TEXTURE_2D, 0, 0, 0, 0,
            source.owner.width, source.owner.height, 1
        );
        GLES20.glFlush();
    }

    public void copyFromReadBuffer(short width, short height) {
        copyFromReadBuffer(width, height, false);
    }

    /* Bild aus dem Lesepuffer des aktuellen Kontexts (Gladio-Server, VirGL) in diese Textur, die der Kontext der
     * Anzeige zeichnet. Gleiche Größe: in das bestehende Bild kopieren statt es jedes Mal neu anzulegen. withFence
     * (beide Kontexte ES 3): dahinter eine Fence, auf die das Zeichnen auf der GPU wartet (waitForCopy). Ohne sie
     * ordnet z. B. der Adreno-Treiber die Befehle der beiden Kontexte nicht: die Anzeige las die Textur, bevor die
     * Kopie fertig war, und zeigte ein älteres Bild (das Bild sprang zurück und wieder vor). */
    public void copyFromReadBuffer(short width, short height, boolean withFence) {
        if (!isAllocated()) allocateTexture(width, height, null);
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId);
        if (copiedWidth == width && copiedHeight == height) {
            GLES20.glCopyTexSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
        }
        else {
            GLES20.glCopyTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, 0, 0, width, height, 0);
            copiedWidth = width;
            copiedHeight = height;
        }
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, 0);
        if (withFence) {
            if (copyFence != 0) GLES30.glDeleteSync(copyFence);
            copyFence = GLES30.glFenceSync(GLES30.GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }
        GLES20.glFlush();
    }

    /* vor dem Zeichnen im Kontext der Anzeige (unter drawable.renderLock): die GPU wartet, bis die letzte Kopie fertig ist */
    public void waitForCopy() {
        if (copyFence == 0) return;
        GLES30.glWaitSync(copyFence, 0, GLES30.GL_TIMEOUT_IGNORED);
        GLES30.glDeleteSync(copyFence);
        copyFence = 0;
    }

    public void destroy() {
        if (textureId > 0) {
            if (!external) {
                int[] textureIds = new int[]{textureId};
                GLES20.glDeleteTextures(textureIds.length, textureIds, 0);
            }
            textureId = 0;
            external = false;
        }
        copiedWidth = copiedHeight = 0;
        if (copyFence != 0) {
            GLES30.glDeleteSync(copyFence);
            copyFence = 0;
        }
    }
}
