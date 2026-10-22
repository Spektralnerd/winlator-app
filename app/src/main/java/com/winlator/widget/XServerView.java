package com.winlator.widget;

import android.annotation.SuppressLint;
import android.content.Context;
import android.opengl.GLSurfaceView;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import com.winlator.core.GPUHelper;
import com.winlator.renderer.GLRenderer;
import com.winlator.xserver.XServer;

@SuppressLint("ViewConstructor")
public class XServerView extends GLSurfaceView {
    private final GLRenderer renderer;

    public XServerView(Context context, XServer xServer) {
        super(context);
        setLayoutParams(new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        // OpenGL ES 3 wie im Original; Geräte, die nur ES 2.0 können (z. B. Mali-400), bekommen
        // einen ES-2-Kontext, die Shader werden dann nach GLSL ES 1.00 übersetzt (ShaderMaterial)
        GLRenderer.glesVersion = GPUHelper.getGLESMajorVersion(context) >= 3 ? 3 : 2;
        setEGLContextClientVersion(GLRenderer.glesVersion);
        setEGLConfigChooser(8, 8, 8, 8, 0, 0);
        setPreserveEGLContextOnPause(true);
        renderer = new GLRenderer(this, xServer);
        setRenderer(renderer);
        setRenderMode(RENDERMODE_WHEN_DIRTY);
    }

    public GLRenderer getRenderer() {
        return renderer;
    }
}
