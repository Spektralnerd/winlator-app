package com.winlator.renderer.material;

import android.graphics.Color;
import android.opengl.GLES20;

import com.winlator.renderer.GLRenderer;

public abstract class ShaderMaterial {
    private static final float INV_255 = 1.0f / 255.0f;
    public int programId;

    public static class Uniform {
        public final String name;
        public int location = -1;

        public Uniform(String name) {
            this.name = name;
        }
    }

    protected static int compileShaders(String vertexShader, String fragmentShader) {
        final boolean glsl300 = GLRenderer.glesVersion >= 3;
        int beginIndex = vertexShader.indexOf("void main() {");
        vertexShader = vertexShader.substring(0, beginIndex) + (glsl300 ?
            "vec2 applyXForm(vec2 p, float xform[6]) {\n" +
                "return vec2(xform[0] * p.x + xform[2] * p.y + xform[4], xform[1] * p.x + xform[3] * p.y + xform[5]);\n" +
            "}\n" :
            // GLSL ES 1.00: als Makro, Arrays als Funktionsparameter kann nicht jeder ES-2-Treiber
            "#define applyXForm(p, xf) vec2(xf[0] * p.x + xf[2] * p.y + xf[4], xf[1] * p.x + xf[3] * p.y + xf[5])\n") +
            vertexShader.substring(beginIndex);

        if (glsl300) {
            vertexShader = "#version 300 es\n" + vertexShader;
            fragmentShader = "#version 300 es\n" + fragmentShader;
        }
        else {
            vertexShader = "#version 100\n" + toGLSL100(vertexShader, false);
            fragmentShader = "#version 100\n" + toGLSL100(fragmentShader, true);
        }

        int programId = GLES20.glCreateProgram();
        int[] compiled = new int[1];

        int vertexShaderId = GLES20.glCreateShader(GLES20.GL_VERTEX_SHADER);
        GLES20.glShaderSource(vertexShaderId, vertexShader);
        GLES20.glCompileShader(vertexShaderId);

        GLES20.glGetShaderiv(vertexShaderId, GLES20.GL_COMPILE_STATUS, compiled, 0);
        if (compiled[0] == 0) {
            throw new RuntimeException("Could not compile vertex shader: \n" + GLES20.glGetShaderInfoLog(vertexShaderId));
        }
        GLES20.glAttachShader(programId, vertexShaderId);

        int fragmentShaderId = GLES20.glCreateShader(GLES20.GL_FRAGMENT_SHADER);
        GLES20.glShaderSource(fragmentShaderId, fragmentShader);
        GLES20.glCompileShader(fragmentShaderId);

        GLES20.glGetShaderiv(fragmentShaderId, GLES20.GL_COMPILE_STATUS, compiled, 0);
        if (compiled[0] == 0) {
            throw new RuntimeException("Could not compile fragment shader: \n" + GLES20.glGetShaderInfoLog(fragmentShaderId));
        }
        GLES20.glAttachShader(programId, fragmentShaderId);

        GLES20.glLinkProgram(programId);

        GLES20.glDeleteShader(vertexShaderId);
        GLES20.glDeleteShader(fragmentShaderId);
        return programId;
    }

    /**
     * Übersetzt die Shader (GLSL ES 3.00 ohne #version-Zeile) für OpenGL-ES-2-Kontexte nach
     * GLSL ES 1.00: in/out werden attribute/varying, texture() wird texture2D(), die Ausgabe
     * geht an gl_FragColor, highp im Fragment-Shader nur wo vorhanden (Mali-400 kann nur mediump).
     */
    private static String toGLSL100(String source, boolean fragment) {
        StringBuilder sb = new StringBuilder();
        if (fragment) sb.append("#define outFragColor gl_FragColor\n");
        for (String line : source.split("\n", -1)) {
            String trimmed = line.trim();
            if (fragment && trimmed.startsWith("layout(") && trimmed.contains(" out ")) continue;

            if (trimmed.startsWith("in ")) {
                line = (fragment ? "varying " : "attribute ") + trimmed.substring(3);
            }
            else if (trimmed.startsWith("out ")) {
                line = "varying " + trimmed.substring(4);
            }
            else if (fragment && trimmed.equals("precision highp float;")) {
                line = "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif";
            }

            sb.append(line.replaceAll("\\btexture\\s*\\(", "texture2D(")).append('\n');
        }
        return sb.toString();
    }

    protected String getVertexShader() {
        return "";
    }

    protected String getFragmentShader() {
        return "";
    }

    public void use() {
        if (programId == 0) programId = compileShaders(getVertexShader(), getFragmentShader());
        GLES20.glUseProgram(programId);
    }

    private int getUniformLocation(Uniform uniform) {
        if (uniform.location != -1) return uniform.location;
        int location = programId != 0 ? GLES20.glGetUniformLocation(programId, uniform.name) : -1;
        uniform.location = location;
        return location;
    }

    public void destroy() {
        GLES20.glDeleteProgram(programId);
        programId = 0;
    }

    public void setUniformColor(Uniform uniform, int color) {
        int location = getUniformLocation(uniform);
        if (location != -1) GLES20.glUniform3f(location, Color.red(color) * INV_255, Color.green(color) * INV_255, Color.blue(color) * INV_255);
    }

    public void setUniformFloat(Uniform uniform, float value) {
        int location = getUniformLocation(uniform);
        if (location != -1) GLES20.glUniform1f(location, value);
    }

    public void setUniformFloatArray(Uniform uniform, float[] values) {
        int location = getUniformLocation(uniform);
        if (location != -1) GLES20.glUniform1fv(location, values.length, values, 0);
    }

    public void setUniformInt(Uniform uniform, int value) {
        int location = getUniformLocation(uniform);
        if (location != -1) GLES20.glUniform1i(location, value);
    }

    public void setUniformBool(Uniform uniform, boolean value) {
        setUniformInt(uniform, value ? 1 : 0);
    }

    public void setUniformVec2(Uniform uniform, float x, float y) {
        int location = getUniformLocation(uniform);
        if (location != -1) GLES20.glUniform2f(location, x, y);
    }
}
