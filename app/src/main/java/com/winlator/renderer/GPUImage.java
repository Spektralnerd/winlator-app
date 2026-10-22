package com.winlator.renderer;

import androidx.annotation.Keep;

import com.winlator.xserver.Drawable;
import com.winlator.xserver.XServer;

import java.nio.ByteBuffer;

public class GPUImage extends Texture {
    private long hardwareBufferPtr;
    private long imageKHRPtr;
    private ByteBuffer virtualData;
    private short stride;
    private boolean locked = false;
    private int nativeHandle;

    private static Boolean supported;

    static {
        System.loadLibrary("winlator");
    }

    /**
     * AHardwareBuffer und EGLImage aus AHardwareBuffer gibt es erst ab Android 8.0, die Bildausgabe
     * braucht dafür außerdem OpenGL ES 3. Ohne sie bleiben Fenster normale Texturen (Inhalt per
     * glTexSubImage2D statt geteiltem Speicher).
     */
    public static boolean isSupported() {
        if (supported == null) supported = isSupportedNative();
        // Kopieren zwischen GPUImages braucht OpenGL ES 3.2 (glCopyImageSubData)
        return supported && GLRenderer.glesVersion >= 3;
    }

    public GPUImage(Drawable owner) {
        this(owner, true, true);
    }

    public GPUImage(Drawable owner, boolean cpuAccess) {
        this(owner, cpuAccess, true);
    }

    public GPUImage(Drawable owner, boolean cpuAccess, boolean useHALPixelFormatBGRA8888) {
        super(owner);
        hardwareBufferPtr = createHardwareBuffer(owner.width, owner.height, cpuAccess, useHALPixelFormatBGRA8888);
        if (cpuAccess && hardwareBufferPtr != 0) {
            virtualData = lockHardwareBuffer(hardwareBufferPtr);
            locked = true;
        }
    }

    @Override
    public void allocateTexture(short width, short height, ByteBuffer data) {
        if (isAllocated()) return;
        super.allocateTexture(width, height, null);
        imageKHRPtr = createImageKHR(hardwareBufferPtr, textureId);
    }

    @Override
    public void updateFromDrawable() {
        if (!isAllocated() && owner != null) allocateTexture(owner.width, owner.height, null);
        needsUpdate = false;
    }

    public short getStride() {
        return stride;
    }

    @Keep
    private void setStride(short stride) {
        this.stride = stride;
    }

    public int getNativeHandle() {
        return nativeHandle;
    }

    @Keep
    private void setNativeHandle(int nativeHandle) {
        this.nativeHandle = nativeHandle;
    }

    public ByteBuffer getVirtualData() {
        return virtualData;
    }

    @Override
    public void destroy() {
        destroyImageKHR(imageKHRPtr);
        destroyHardwareBuffer(hardwareBufferPtr, locked);
        virtualData = null;
        imageKHRPtr = 0;
        hardwareBufferPtr = 0;
        super.destroy();
    }

    public long getHardwareBufferPtr() {
        return hardwareBufferPtr;
    }

    /** @return null, wenn das Gerät keine GPUImages kann (siehe {@link #isSupported()}) */
    public static GPUImage createOrObtain(XServer xServer, Drawable drawable, boolean cpuAccess, boolean useHALPixelFormatBGRA8888) {
        if (!isSupported()) return null;
        final Texture texture = drawable.getTexture();
        if (!(texture instanceof GPUImage)) {
            xServer.getRenderer().xServerView.queueEvent(texture::destroy);
            GPUImage gpuImage = new GPUImage(drawable, cpuAccess, useHALPixelFormatBGRA8888);
            drawable.setTexture(gpuImage);
            return gpuImage;
        }
        else return (GPUImage)texture;
    }

    private static native boolean isSupportedNative();

    private native long createHardwareBuffer(short width, short height, boolean cpuAccess, boolean useHALPixelFormatBGRA8888);

    private native void destroyHardwareBuffer(long hardwareBufferPtr, boolean locked);

    private native ByteBuffer lockHardwareBuffer(long hardwareBufferPtr);

    private native long createImageKHR(long hardwareBufferPtr, int textureId);

    private native void destroyImageKHR(long imageKHRPtr);
}