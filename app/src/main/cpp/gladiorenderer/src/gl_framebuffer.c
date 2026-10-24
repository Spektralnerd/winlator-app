#include "gl_framebuffer.h"
#include "gl_context.h"
#include "gl_formats.h"
#include "gl_es2.h"

/* ES 2.0 kennt nur GL_FRAMEBUFFER; Lese- und Zeichenziel sind dasselbe. */
static GLenum fbTarget(GLenum target) {
    return GLES2 ? GL_FRAMEBUFFER : target;
}

static GLuint maxFramebufferId = 1;

static GLFramebuffer* createNamedFramebuffer(GLuint id) {
    GLFramebuffer* framebuffer = calloc(1, sizeof(GLFramebuffer));
    framebuffer->ownerId = currentRenderer->contextId;
    framebuffer->id = id;
    SparseArray_put(currentRenderer->clientState.framebuffers, id, framebuffer);
    return framebuffer;
}

GLuint GLFramebuffer_create() {
    GLX_CONTEXT_LOCK();
    GLuint id = maxFramebufferId++;
    createNamedFramebuffer(id);
    GLX_CONTEXT_UNLOCK();
    return id;
}

static void assignFBAttachment(GLenum target, GLuint attachment, FBAttachmentInfo* attachmentInfo) {
    if (attachmentInfo->type == 0) return;
    if (GLES2) {
        target = GL_FRAMEBUFFER;
        if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            /* ES 2.0: ein gepackter Tiefen-/Stencil-Puffer wird an beide Punkte gehängt */
            assignFBAttachment(target, GL_DEPTH_ATTACHMENT, attachmentInfo);
            if (gles.packedDepthStencil) assignFBAttachment(target, GL_STENCIL_ATTACHMENT, attachmentInfo);
            return;
        }
        if (attachment > GL_COLOR_ATTACHMENT0 && attachment <= GL_COLOR_ATTACHMENT31) return;
    }
    if (attachmentInfo->type == GL_RENDERBUFFER) {
        glFramebufferRenderbuffer(target, attachment, attachmentInfo->type, attachmentInfo->id);
    }
    else glFramebufferTexture2D(target, attachment, attachmentInfo->type, attachmentInfo->id, attachmentInfo->level);
}

static void recreateFramebuffer(GLenum target, GLFramebuffer* framebuffer) {
    glBindFramebuffer(fbTarget(target), framebuffer->id);

    for (int i = 0; i < MAX_FB_COLOR_ATTACHMENTS; i++) {
        assignFBAttachment(target, GL_COLOR_ATTACHMENT0 + i, &framebuffer->colorAttachment[i]);
    }

    assignFBAttachment(target, GL_DEPTH_ATTACHMENT, &framebuffer->depthAttachment);
    if (!GLES2 || gles.packedDepthStencil || framebuffer->stencilAttachment.id != framebuffer->depthAttachment.id) {
        assignFBAttachment(target, GL_STENCIL_ATTACHMENT, &framebuffer->stencilAttachment);
    }

    if (GLES2) return;
    if (framebuffer->flags & FLAG_READ_BUFFER_NONE) glReadBuffer(GL_NONE);
    for (int i = 0, j = GETEXP(FLAG_READ_BUFFER_COLOR_ATTACHMENT); i < MAX_FB_COLOR_ATTACHMENTS; i++, j++) {
        if (framebuffer->flags & (1<<j)) glReadBuffer(GL_COLOR_ATTACHMENT0 + i);
    }

    if (framebuffer->flags & FLAG_DRAW_BUFFER_NONE) {
        GLenum none = GL_NONE;
        glDrawBuffers(1, &none);
    }

    GLenum drawBufs[MAX_FB_COLOR_ATTACHMENTS];
    int numDrawBufs = 0;
    for (int i = 0, j = GETEXP(FLAG_DRAW_BUFFER_COLOR_ATTACHMENT); i < MAX_FB_COLOR_ATTACHMENTS; i++, j++) {
        if (framebuffer->flags & (1<<j)) drawBufs[numDrawBufs++] = GL_COLOR_ATTACHMENT0 + i;
    }
    if (numDrawBufs > 0) glDrawBuffers(numDrawBufs, drawBufs);
}

void GLFramebuffer_bind(GLenum target, GLuint id) {
    GLX_CONTEXT_LOCK();
    if (id == 0) id = currentRenderer->displayBuffer;
    GLFramebuffer* framebuffer = SparseArray_get(currentRenderer->clientState.framebuffers, id);
    if (!framebuffer) framebuffer = createNamedFramebuffer(id);

    if (target == GL_FRAMEBUFFER || GLES2) {
        ARRAYS_FILL(currentRenderer->clientState.framebuffer, MAX_FRAMEBUFFER_TARGETS, id);
    }
    else currentRenderer->clientState.framebuffer[indexOfGLTarget(target)] = id;

    if (currentRenderer->contextId != framebuffer->ownerId) {
        recreateFramebuffer(target, framebuffer);
        framebuffer->ownerId = currentRenderer->contextId;
    }
    else glBindFramebuffer(fbTarget(target), framebuffer->id);
    GLX_CONTEXT_UNLOCK();
}

GLFramebuffer* GLFramebuffer_getBound(GLenum target) {
    GLX_CONTEXT_LOCK();
    GLuint id = currentRenderer->clientState.framebuffer[indexOfGLTarget(target)];
    GLFramebuffer* framebuffer = SparseArray_get(currentRenderer->clientState.framebuffers, id);
    GLX_CONTEXT_UNLOCK();
    return framebuffer;
}

void GLFramebuffer_setAttachment(GLenum target, GLenum attachment, GLenum objectType, GLuint objectId, uint8_t level) {
    GLFramebuffer* framebuffer = GLFramebuffer_getBound(target);

    if (attachment >= GL_COLOR_ATTACHMENT0 && attachment <= GL_COLOR_ATTACHMENT31) {
        int index = attachment - GL_COLOR_ATTACHMENT0;
        framebuffer->colorAttachment[index].type = objectType;
        framebuffer->colorAttachment[index].id = objectId;
        framebuffer->colorAttachment[index].level = level;
        assignFBAttachment(target, attachment, &framebuffer->colorAttachment[index]);
    }
    else {
        if (attachment == GL_DEPTH_ATTACHMENT ||
            attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            framebuffer->depthAttachment.type = objectType;
            framebuffer->depthAttachment.id = objectId;
            framebuffer->depthAttachment.level = level;
            assignFBAttachment(target, attachment, &framebuffer->depthAttachment);
        }
        if (attachment == GL_STENCIL_ATTACHMENT ||
            attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            framebuffer->stencilAttachment.type = objectType;
            framebuffer->stencilAttachment.id = objectId;
            framebuffer->stencilAttachment.level = level;
            /* bei DEPTH_STENCIL hat der Aufruf oben in ES 2.0 schon beide Punkte gesetzt */
            if (!(GLES2 && attachment == GL_DEPTH_STENCIL_ATTACHMENT)) assignFBAttachment(target, attachment, &framebuffer->stencilAttachment);
        }
    }
}

void GLFramebuffer_delete(GLuint id) {
    for (int i = 0; i < MAX_FRAMEBUFFER_TARGETS; i++) {
        if (id == currentRenderer->clientState.framebuffer[i]) {
            GLFramebuffer_bind(GL_FRAMEBUFFER, 0);
        }
    }

    GLX_CONTEXT_LOCK();
    GLFramebuffer* framebuffer = SparseArray_get(currentRenderer->clientState.framebuffers, id);
    if (framebuffer && framebuffer->ownerId == currentRenderer->contextId) {
        glDeleteFramebuffers(1, &framebuffer->id);
        SparseArray_remove(currentRenderer->clientState.framebuffers, id);
        free(framebuffer);
    }
    GLX_CONTEXT_UNLOCK();
}

void GLFramebuffer_setReadBuffer(GLenum src) {
    GLFramebuffer* framebuffer = GLFramebuffer_getBound(GL_FRAMEBUFFER);
    BITMASK_UNSET(framebuffer->flags, FLAG_READ_BUFFER_NONE);

    for (int i = 0, j = GETEXP(FLAG_READ_BUFFER_COLOR_ATTACHMENT); i < MAX_FB_COLOR_ATTACHMENTS; i++, j++) {
        BITMASK_UNSET(framebuffer->flags, (1<<j));
    }

    bool success = false;
    if (src == GL_NONE) {
        BITMASK_SET(framebuffer->flags, FLAG_READ_BUFFER_NONE);
        success = true;
    }
    else if (src >= GL_COLOR_ATTACHMENT0 && src <= GL_COLOR_ATTACHMENT31) {
        int index = (src - GL_COLOR_ATTACHMENT0) + GETEXP(FLAG_READ_BUFFER_COLOR_ATTACHMENT);
        BITMASK_SET(framebuffer->flags, (1<<index));
        success = true;
    }

    if (success && !GLES2) glReadBuffer(src);
}

void GLFramebuffer_setDrawBuffers(GLuint count, GLenum* dst) {
    GLFramebuffer* framebuffer = GLFramebuffer_getBound(GL_FRAMEBUFFER);
    BITMASK_UNSET(framebuffer->flags, FLAG_DRAW_BUFFER_NONE);

    for (int i = 0, j = GETEXP(FLAG_DRAW_BUFFER_COLOR_ATTACHMENT); i < MAX_FB_COLOR_ATTACHMENTS; i++, j++) {
        BITMASK_UNSET(framebuffer->flags, (1<<j));
    }

    bool success = false;
    for (int i = 0; i < count; i++) {
        if (dst[i] == GL_NONE) {
            BITMASK_SET(framebuffer->flags, FLAG_DRAW_BUFFER_NONE);
            success = true;
        }
        else if (dst[i] >= GL_COLOR_ATTACHMENT0 && dst[i] <= GL_COLOR_ATTACHMENT31) {
            int index = (dst[i] - GL_COLOR_ATTACHMENT0) + GETEXP(FLAG_DRAW_BUFFER_COLOR_ATTACHMENT);
            BITMASK_SET(framebuffer->flags, (1<<index));
            success = true;
        }
    }
    if (success && !GLES2) glDrawBuffers(count, dst);
}

void GLFramebuffer_getParamsv(GLenum target, GLenum attachment, GLenum pname, GLint *params) {
    *params = 0;
    if (attachment == GL_FRONT_LEFT ||
        attachment == GL_FRONT_RIGHT ||
        attachment == GL_BACK_LEFT ||
        attachment == GL_BACK_RIGHT ||
        attachment == GL_DEPTH ||
        attachment == GL_STENCIL) {

        GLFormatInfo* framebufferFormat = GLFormats_queryInternalformat(GL_TEXTURE_2D, PREFERRED_FRAMEBUFFER_FORMAT, 0, 0, NULL);
        GLFormatInfo* renderbufferFormat = GLFormats_queryInternalformat(GL_RENDERBUFFER, PREFERRED_RENDERBUFFER_FORMAT, 0, 0, NULL);

        switch (pname) {
            case  GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE:
                *params = framebufferFormat->redSize;
                break;
            case  GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE:
                *params = framebufferFormat->greenSize;
                break;
            case  GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE:
                *params = framebufferFormat->blueSize;
                break;
            case  GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE:
                *params = framebufferFormat->alphaSize;
                break;
            case  GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE:
                *params = renderbufferFormat->stencilSize;
                break;
            case  GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE:
                *params = renderbufferFormat->depthSize;
                break;
        }
    }
    else if (GLES2) {
        switch (pname) {
            case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:
            case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:
            case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:
            case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
                if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) attachment = GL_DEPTH_ATTACHMENT;
                glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment, pname, params);
                break;
            case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE:
            case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE:
            case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE:
            case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE:
            case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE:
                *params = 8;
                break;
            case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE:
                *params = gles.depth24 || gles.packedDepthStencil ? 24 : 16;
                break;
            case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE:
                *params = GL_UNSIGNED_NORMALIZED;
                break;
            case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING:
                *params = GL_LINEAR;
                break;
        }
    }
    else glGetFramebufferAttachmentParameteriv(target, attachment, pname, params);
}