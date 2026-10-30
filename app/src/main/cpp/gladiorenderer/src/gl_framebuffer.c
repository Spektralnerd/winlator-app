#include "gl_framebuffer.h"
#include "gl_context.h"
#include "gl_formats.h"
#include "gl_es2.h"
#include "gl_texture.h"

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

/* ---- Größen der Renderbuffer (ES 2.0) ---- */
typedef struct RenderbufferSize { int width, height; } RenderbufferSize;
static SparseArray renderbufferSizes = {0};

void GLFramebuffer_noteRenderbufferSize(GLuint renderbuffer, int width, int height) {
    GLX_CONTEXT_LOCK();
    RenderbufferSize* size = SparseArray_get(&renderbufferSizes, renderbuffer);
    if (!size) {
        size = calloc(1, sizeof(RenderbufferSize));
        SparseArray_put(&renderbufferSizes, renderbuffer, size);
    }
    size->width = width;
    size->height = height;
    GLX_CONTEXT_UNLOCK();
}

/* Größe eines Anhangs, 0 wenn unbekannt */
static void attachmentSize(GLFramebuffer* framebuffer, FBAttachmentInfo* info, int* width, int* height) {
    *width = *height = 0;
    if (info->type == 0 || info->id == 0) return;
    if (info->type == GL_RENDERBUFFER) {
        RenderbufferSize* size = SparseArray_get(&renderbufferSizes, info->id);
        if (size) { *width = size->width; *height = size->height; }
    }
    else if (info->texture && info->texture->width > 0) {
        *width = MAX(1, info->texture->width >> info->level);
        *height = MAX(1, info->texture->height >> info->level);
    }
    else if (framebuffer->id == currentRenderer->displayBuffer) {
        *width = currentRenderer->displaySize[0];
        *height = currentRenderer->displaySize[1];
    }
}

/* ES 2.0: Tiefe/Stencil anhängen; bei anderer Größe als der Farbanhang einen eigenen
 * Renderbuffer in Farbgröße (Anwendungen hängen z. B. den großen Tiefenpuffer des
 * Fensters an kleine Render-Target-Texturen – in ES 2.0 wäre das FBO unvollständig). */
static void attachDepthStencilES2(GLFramebuffer* framebuffer) {
    FBAttachmentInfo* depth = framebuffer->depthAttachment.type ? &framebuffer->depthAttachment : &framebuffer->stencilAttachment;
    int colorWidth, colorHeight, depthWidth, depthHeight;
    attachmentSize(framebuffer, &framebuffer->colorAttachment[0], &colorWidth, &colorHeight);
    attachmentSize(framebuffer, depth, &depthWidth, &depthHeight);
    bool mismatch = depth->type != 0 && colorWidth > 0 && depthWidth > 0 && (colorWidth != depthWidth || colorHeight != depthHeight);

    if (mismatch) {
        bool wantStencil = framebuffer->stencilAttachment.type != 0 && gles.packedDepthStencil;
        if (framebuffer->substituteDepth == 0 || framebuffer->substituteWidth != colorWidth || framebuffer->substituteHeight != colorHeight) {
            if (framebuffer->substituteDepth == 0) glGenRenderbuffers(1, &framebuffer->substituteDepth);
            GLint savedRenderbuffer = 0;
            glGetIntegerv(GL_RENDERBUFFER_BINDING, &savedRenderbuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, framebuffer->substituteDepth);
            GLenum format = gles.packedDepthStencil ? GLES_renderbufferFormat(GL_DEPTH24_STENCIL8) : GLES_renderbufferFormat(GL_DEPTH_COMPONENT24);
            glRenderbufferStorage(GL_RENDERBUFFER, format, colorWidth, colorHeight);
            glBindRenderbuffer(GL_RENDERBUFFER, savedRenderbuffer);
            framebuffer->substituteWidth = colorWidth;
            framebuffer->substituteHeight = colorHeight;
            framebuffer->substituteFresh = true;
        }
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, framebuffer->substituteDepth);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, wantStencil ? framebuffer->substituteDepth : 0);
        if (framebuffer->substituteFresh) {
            /* neuer Puffer: wie ein frisch gelöschter Tiefenpuffer beginnen (Scissor darf nicht stören) */
            framebuffer->substituteFresh = false;
            GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
            GLboolean depthMask = GL_TRUE;
            glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
            GLint stencilMask = 0xff;
            glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilMask);
            GLfloat clearDepth = 1.0f;
            glGetFloatv(GL_DEPTH_CLEAR_VALUE, &clearDepth);
            GLint clearStencil = 0;
            glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &clearStencil);
            if (scissor) glDisable(GL_SCISSOR_TEST);
            glDepthMask(GL_TRUE);
            glStencilMask(0xffffffff);
            glClearDepthf(1.0f);
            glClearStencil(0);
            glClear(GL_DEPTH_BUFFER_BIT | (wantStencil ? GL_STENCIL_BUFFER_BIT : 0));
            glClearDepthf(clearDepth);
            glClearStencil(clearStencil);
            glDepthMask(depthMask);
            glStencilMask(stencilMask);
            if (scissor) glEnable(GL_SCISSOR_TEST);
        }
        if (!framebuffer->substituteActive) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                println("gladio:es2: Tiefenanhang %dx%d passt nicht zum Farbanhang %dx%d, eigener Tiefenpuffer", depthWidth, depthHeight, colorWidth, colorHeight);
            }
        }
        framebuffer->substituteActive = true;
        return;
    }

    framebuffer->substituteActive = false;
    if (framebuffer->depthAttachment.type == 0) glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
    else assignFBAttachment(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, &framebuffer->depthAttachment);
    if (framebuffer->stencilAttachment.type == 0) glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
    else if (gles.packedDepthStencil || framebuffer->stencilAttachment.id != framebuffer->depthAttachment.id) {
        assignFBAttachment(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, &framebuffer->stencilAttachment);
    }
}

static void recreateFramebuffer(GLenum target, GLFramebuffer* framebuffer) {
    glBindFramebuffer(fbTarget(target), framebuffer->id);

    for (int i = 0; i < MAX_FB_COLOR_ATTACHMENTS; i++) {
        assignFBAttachment(target, GL_COLOR_ATTACHMENT0 + i, &framebuffer->colorAttachment[i]);
    }

    if (GLES2) {
        attachDepthStencilES2(framebuffer);
        return;
    }
    assignFBAttachment(target, GL_DEPTH_ATTACHMENT, &framebuffer->depthAttachment);
    assignFBAttachment(target, GL_STENCIL_ATTACHMENT, &framebuffer->stencilAttachment);

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

static GLboolean appColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};

/* ES 2.0: Farbmaske der Anwendung, bei glDrawBuffer(GL_NONE) des gebundenen Zeichen-FBOs ganz zu
 * (Variante ohne Sperre für Aufrufer, die GLX_CONTEXT_LOCK schon halten) */
static void applyColorMaskES2Locked(void) {
    GLuint drawId = currentRenderer->clientState.framebuffer[indexOfGLTarget(GL_DRAW_FRAMEBUFFER)];
    GLFramebuffer* framebuffer = SparseArray_get(currentRenderer->clientState.framebuffers, drawId);
    bool none = framebuffer && (framebuffer->flags & FLAG_DRAW_BUFFER_NONE);
    if (none) glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    else glColorMask(appColorMask[0], appColorMask[1], appColorMask[2], appColorMask[3]);
}

static void applyColorMaskES2(void) {
    GLX_CONTEXT_LOCK();
    applyColorMaskES2Locked();
    GLX_CONTEXT_UNLOCK();
}

void GLFramebuffer_setColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
    appColorMask[0] = red; appColorMask[1] = green; appColorMask[2] = blue; appColorMask[3] = alpha;
    if (GLES2) applyColorMaskES2();
    else glColorMask(red, green, blue, alpha);
}

void GLFramebuffer_bind(GLenum target, GLuint id) {
    GLX_CONTEXT_LOCK();
    if (id == 0) id = currentRenderer->displayBuffer;
    GLFramebuffer* framebuffer = SparseArray_get(currentRenderer->clientState.framebuffers, id);
    if (!framebuffer) framebuffer = createNamedFramebuffer(id);
    GLuint* slots = currentRenderer->clientState.framebuffer;

    if (GLES2) {
        /* Lese- und Zeichen-FBO getrennt führen; gebunden bleibt das Zeichen-FBO */
        bool bindGL = true;
        if (target == GL_READ_FRAMEBUFFER) {
            slots[indexOfGLTarget(GL_READ_FRAMEBUFFER)] = id;
            bindGL = false;
        }
        else if (target == GL_DRAW_FRAMEBUFFER) {
            slots[indexOfGLTarget(GL_DRAW_FRAMEBUFFER)] = id;
            slots[indexOfGLTarget(GL_FRAMEBUFFER)] = id;
        }
        else ARRAYS_FILL(slots, MAX_FRAMEBUFFER_TARGETS, id);

        if (currentRenderer->contextId != framebuffer->ownerId) {
            recreateFramebuffer(GL_FRAMEBUFFER, framebuffer);
            framebuffer->ownerId = currentRenderer->contextId;
            if (!bindGL) glBindFramebuffer(GL_FRAMEBUFFER, slots[indexOfGLTarget(GL_DRAW_FRAMEBUFFER)]);
        }
        else if (bindGL) glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->id);
        if (bindGL) applyColorMaskES2Locked();
        GLX_CONTEXT_UNLOCK();
        return;
    }

    if (target == GL_FRAMEBUFFER) {
        ARRAYS_FILL(slots, MAX_FRAMEBUFFER_TARGETS, id);
    }
    else if (target == GL_DRAW_FRAMEBUFFER) {
        /* GL_FRAMEBUFFER meint bei Anhängen und Statusabfragen das Zeichen-FBO */
        slots[indexOfGLTarget(GL_DRAW_FRAMEBUFFER)] = id;
        slots[indexOfGLTarget(GL_FRAMEBUFFER)] = id;
    }
    else if (target == GL_READ_FRAMEBUFFER) slots[indexOfGLTarget(target)] = id;
    else {
        /* unbekanntes Ziel: indexOfGLTarget liefert 255 (vorher Schreibzugriff hinter clientState.framebuffer) */
        GLX_CONTEXT_UNLOCK();
        return;
    }

    if (currentRenderer->contextId != framebuffer->ownerId) {
        recreateFramebuffer(target, framebuffer);
        framebuffer->ownerId = currentRenderer->contextId;
    }
    else glBindFramebuffer(target, framebuffer->id);
    GLX_CONTEXT_UNLOCK();
}

bool GLFramebuffer_beginReadAccess(void) {
    if (!GLES2) return false;
    GLuint* slots = currentRenderer->clientState.framebuffer;
    GLuint readId = slots[indexOfGLTarget(GL_READ_FRAMEBUFFER)];
    GLuint drawId = slots[indexOfGLTarget(GL_DRAW_FRAMEBUFFER)];
    if (readId == drawId) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, readId);
    return true;
}

void GLFramebuffer_endReadAccess(bool rebound) {
    if (!rebound) return;
    glBindFramebuffer(GL_FRAMEBUFFER, currentRenderer->clientState.framebuffer[indexOfGLTarget(GL_DRAW_FRAMEBUFFER)]);
}

/* ES 2.0: Operationen auf dem Lese-FBO brauchen es kurz gebunden */
static bool beginTargetAccess(GLenum target) {
    return GLES2 && target == GL_READ_FRAMEBUFFER && GLFramebuffer_beginReadAccess();
}

GLenum GLFramebuffer_checkStatus(GLenum target) {
    bool rebound = beginTargetAccess(target);
    GLenum status = glCheckFramebufferStatus(fbTarget(target));
    GLFramebuffer_endReadAccess(rebound);
    return status;
}

GLFramebuffer* GLFramebuffer_getBound(GLenum target) {
    if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER) return NULL;
    GLX_CONTEXT_LOCK();
    GLuint id = currentRenderer->clientState.framebuffer[indexOfGLTarget(target)];
    GLFramebuffer* framebuffer = SparseArray_get(currentRenderer->clientState.framebuffers, id);
    GLX_CONTEXT_UNLOCK();
    return framebuffer;
}

void GLFramebuffer_setAttachment(GLenum target, GLenum attachment, GLenum objectType, GLuint objectId, uint8_t level, struct GLTexture* texture) {
    GLFramebuffer* framebuffer = GLFramebuffer_getBound(target);
    if (!framebuffer) return;
    bool rebound = beginTargetAccess(target);

    if (attachment >= GL_COLOR_ATTACHMENT0 && attachment <= GL_COLOR_ATTACHMENT31) {
        int index = attachment - GL_COLOR_ATTACHMENT0;
        framebuffer->colorAttachment[index].type = objectType;
        framebuffer->colorAttachment[index].id = objectId;
        framebuffer->colorAttachment[index].level = level;
        framebuffer->colorAttachment[index].texture = texture;
        assignFBAttachment(target, attachment, &framebuffer->colorAttachment[index]);
        /* ES 2.0: die Tiefe muss zur (neuen) Farbgröße passen */
        if (GLES2 && index == 0 && (framebuffer->depthAttachment.type || framebuffer->stencilAttachment.type)) attachDepthStencilES2(framebuffer);
    }
    else {
        if (attachment == GL_DEPTH_ATTACHMENT ||
            attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            framebuffer->depthAttachment.type = objectType;
            framebuffer->depthAttachment.id = objectId;
            framebuffer->depthAttachment.level = level;
            framebuffer->depthAttachment.texture = texture;
            if (!GLES2) assignFBAttachment(target, attachment, &framebuffer->depthAttachment);
        }
        if (attachment == GL_STENCIL_ATTACHMENT ||
            attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            framebuffer->stencilAttachment.type = objectType;
            framebuffer->stencilAttachment.id = objectId;
            framebuffer->stencilAttachment.level = level;
            framebuffer->stencilAttachment.texture = texture;
            if (!GLES2) assignFBAttachment(target, attachment, &framebuffer->stencilAttachment);
        }
        if (GLES2) attachDepthStencilES2(framebuffer);
    }
    GLFramebuffer_endReadAccess(rebound);
}

void GLFramebuffer_delete(GLuint id) {
    /* wie GL: ein gebundenes FBO löst sich beim Löschen nur von den Zielen, an denen es hängt */
    GLuint* slots = currentRenderer->clientState.framebuffer;
    bool boundDraw = id == slots[indexOfGLTarget(GL_DRAW_FRAMEBUFFER)] || id == slots[indexOfGLTarget(GL_FRAMEBUFFER)];
    bool boundRead = id == slots[indexOfGLTarget(GL_READ_FRAMEBUFFER)];
    if (boundDraw && boundRead) GLFramebuffer_bind(GL_FRAMEBUFFER, 0);
    else if (boundDraw) GLFramebuffer_bind(GLES2 ? GL_FRAMEBUFFER : GL_DRAW_FRAMEBUFFER, 0);
    else if (boundRead) GLFramebuffer_bind(GL_READ_FRAMEBUFFER, 0);

    GLX_CONTEXT_LOCK();
    GLFramebuffer* framebuffer = SparseArray_get(currentRenderer->clientState.framebuffers, id);
    if (framebuffer && framebuffer->ownerId == currentRenderer->contextId) {
        glDeleteFramebuffers(1, &framebuffer->id);
        if (framebuffer->substituteDepth) glDeleteRenderbuffers(1, &framebuffer->substituteDepth);
        SparseArray_remove(currentRenderer->clientState.framebuffers, id);
        free(framebuffer);
    }
    GLX_CONTEXT_UNLOCK();
}

void GLFramebuffer_setReadBuffer(GLenum src) {
    GLFramebuffer* framebuffer = GLFramebuffer_getBound(GL_READ_FRAMEBUFFER);
    if (!framebuffer) return;
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
    GLFramebuffer* framebuffer = GLFramebuffer_getBound(GL_DRAW_FRAMEBUFFER);
    if (!framebuffer) return;
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
    if (!GLES2) {
        if (success) glDrawBuffers(count, dst);
    }
    else if (framebuffer == GLFramebuffer_getBound(GL_DRAW_FRAMEBUFFER)) applyColorMaskES2();
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
            case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE: {
                if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) attachment = GL_DEPTH_ATTACHMENT;
                bool rebound = beginTargetAccess(target);
                glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment, pname, params);
                GLFramebuffer_endReadAccess(rebound);
                break;
            }
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
void GLFramebuffer_clear(GLbitfield mask) {
    glClear(mask);
    if (!GLES2 || !(mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))) return;

    /* ES 2.0: Löscht die Anwendung den Tiefenpuffer über ein FBO, das ihn direkt trägt, müssen die
     * Ersatz-Renderbuffer der FBOs, die denselben Tiefenanhang (in Farbgröße) vertreten, mitgelöscht
     * werden – wined3d löscht z. B. die große Tiefentextur allein und zeichnet dann in das kleine
     * Render-Target, dessen FBO unseren Ersatzpuffer nutzt. */
    GLX_CONTEXT_LOCK();
    GLuint drawId = currentRenderer->clientState.framebuffer[indexOfGLTarget(GL_DRAW_FRAMEBUFFER)];
    GLFramebuffer* framebuffer = SparseArray_get(currentRenderer->clientState.framebuffers, drawId);
    if (!framebuffer || framebuffer->substituteActive) { GLX_CONTEXT_UNLOCK(); return; }
    FBAttachmentInfo* depth = framebuffer->depthAttachment.type ? &framebuffer->depthAttachment : &framebuffer->stencilAttachment;
    if (!depth->type) { GLX_CONTEXT_UNLOCK(); return; }

    bool rebound = false;
    SparseArray* framebuffers = currentRenderer->clientState.framebuffers;
    for (int i = 0; i < framebuffers->size; i++) {
        GLFramebuffer* other = framebuffers->entries[i].value;
        if (!other || other == framebuffer || !other->substituteActive || other->ownerId != currentRenderer->contextId) continue;
        FBAttachmentInfo* otherDepth = other->depthAttachment.type ? &other->depthAttachment : &other->stencilAttachment;
        if (otherDepth->type != depth->type || otherDepth->id != depth->id) continue;
        glBindFramebuffer(GL_FRAMEBUFFER, other->id);
        glClear(mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));
        rebound = true;
    }
    if (rebound) glBindFramebuffer(GL_FRAMEBUFFER, drawId);
    GLX_CONTEXT_UNLOCK();
}

/* ------------------------------------------------------------------ Blit-Nachbildung (ES 2.0) */

static GLuint blitProgram = 0;
static GLint blitSamplerLocation = -1;
static GLuint blitTempTexture = 0;

static bool ensureBlitProgram(void) {
    if (blitProgram && glIsProgram(blitProgram)) return true;
    static const char* vsSource =
        "attribute vec2 aPos;\nattribute vec2 aTex;\nvarying vec2 vTex;\n"
        "void main() { vTex = aTex; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    static const char* fsSource =
        "precision mediump float;\nvarying vec2 vTex;\nuniform sampler2D uTex;\n"
        "void main() { gl_FragColor = texture2D(uTex, vTex); }\n";
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs, 1, &vsSource, NULL);
    glShaderSource(fs, 1, &fsSource, NULL);
    glCompileShader(vs);
    glCompileShader(fs);
    blitProgram = glCreateProgram();
    glAttachShader(blitProgram, vs);
    glAttachShader(blitProgram, fs);
    glBindAttribLocation(blitProgram, 0, "aPos");
    glBindAttribLocation(blitProgram, 1, "aTex");
    glLinkProgram(blitProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(blitProgram, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512] = {0};
        glGetProgramInfoLog(blitProgram, sizeof(log) - 1, NULL, log);
        println("gladio:es2: Blit-Programm nicht gelinkt: %s", log);
        glDeleteProgram(blitProgram);
        blitProgram = 0;
        return false;
    }
    blitSamplerLocation = glGetUniformLocation(blitProgram, "uTex");
    return true;
}

typedef struct SavedAttrib {
    GLint enabled, size, stride, type, normalized, buffer;
    void* pointer;
} SavedAttrib;

static void saveAttrib(GLuint index, SavedAttrib* a) {
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a->enabled);
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a->size);
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a->stride);
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a->type);
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a->normalized);
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a->buffer);
    glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a->pointer);
}

static void restoreAttrib(GLuint index, const SavedAttrib* a) {
    glBindBuffer(GL_ARRAY_BUFFER, a->buffer);
    if (a->size > 0 && a->type != 0) glVertexAttribPointer(index, a->size, a->type, a->normalized, a->stride, a->pointer);
    if (a->enabled) glEnableVertexAttribArray(index);
    else glDisableVertexAttribArray(index);
}

/* Farbanhang, aus dem gelesen bzw. in den gezeichnet wird (Index nach GL_READ_BUFFER/GL_DRAW_BUFFER0) */
static FBAttachmentInfo* activeColorAttachment(GLenum target) {
    GLuint id = currentRenderer->clientState.framebuffer[indexOfGLTarget(target)];
    GLFramebuffer* framebuffer = SparseArray_get(currentRenderer->clientState.framebuffers, id);
    if (!framebuffer) return NULL;
    GLint buffer = GL_NONE;
    glGetIntegerv(target == GL_READ_FRAMEBUFFER ? GL_READ_BUFFER : GL_DRAW_BUFFER0, &buffer);
    if (buffer < GL_COLOR_ATTACHMENT0 || buffer >= GL_COLOR_ATTACHMENT0 + MAX_FB_COLOR_ATTACHMENTS) return NULL;
    return &framebuffer->colorAttachment[buffer - GL_COLOR_ATTACHMENT0];
}

/* ES 3: ungestreckter Farb-Blit aus einem linearen Anhang in eine sRGB-Textur. wined3d füllt so die sRGB-Fassung
 * einer Textur (D3DSAMP_SRGBTEXTURE); Desktop-GL kopiert dabei roh (GL_FRAMEBUFFER_SRGB aus). ES wandelt beim
 * Schreiben in ein sRGB-Ziel immer um – die Bytes wären verändert und das Lesen mit sRGB brächte wieder den linearen
 * Wert (D3D9-Test d3d9srgb: A8R8G8B8 131 statt 59). Deshalb roh über glReadPixels und glTexSubImage2D; der Treiber
 * stellt BGRA-Speicher dabei auf RGBA um. */
static bool blitRawToSrgbTexture(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask) {
    if (mask != GL_COLOR_BUFFER_BIT) return false;
    int width = srcX1 - srcX0, height = srcY1 - srcY0;
    if (width <= 0 || height <= 0 || dstX1 - dstX0 != width || dstY1 - dstY0 != height || srcX0 < 0 || srcY0 < 0 || dstX0 < 0 || dstY0 < 0) return false;
    FBAttachmentInfo* dst = activeColorAttachment(GL_DRAW_FRAMEBUFFER);
    if (!dst || dst->type != GL_TEXTURE_2D || !dst->texture || !dst->id || !isSrgbTextureFormat(dst->texture->originFormat)) return false;
    FBAttachmentInfo* src = activeColorAttachment(GL_READ_FRAMEBUFFER);
    if (!src || (src->texture && isSrgbTextureFormat(src->texture->originFormat))) return false; /* sRGB nach sRGB: Blit bleibt roh */
    if ((int64_t)width * height > 4096 * 4096) return false;

    void* pixels = malloc((size_t)width * height * 4);
    if (!pixels) return false;

    GLint packRowLength = 0, packSkipRows = 0, packSkipPixels = 0, packAlignment = 4, packBuffer = 0;
    glGetIntegerv(GL_PACK_ROW_LENGTH, &packRowLength);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &packSkipRows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &packSkipPixels);
    glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (packBuffer) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glReadPixels(srcX0, srcY0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (packBuffer) glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
    glPixelStorei(GL_PACK_ROW_LENGTH, packRowLength);
    glPixelStorei(GL_PACK_SKIP_ROWS, packSkipRows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, packSkipPixels);
    glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);

    GLint unpackRowLength = 0, unpackSkipRows = 0, unpackSkipPixels = 0, unpackAlignment = 4, boundTexture = 0;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpackRowLength);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpackSkipRows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpackSkipPixels);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundTexture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, dst->id);
    glTexSubImage2D(GL_TEXTURE_2D, dst->level, dstX0, dstY0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, boundTexture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, unpackRowLength);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, unpackSkipRows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpackSkipPixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
    free(pixels);
    return true;
}

void GLFramebuffer_blit(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
    if (!GLES2) {
        if (blitRawToSrgbTexture(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask)) return;
        glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
        return;
    }
    if (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            println("gladio:es2: glBlitFramebuffer für Tiefe/Stencil wird nicht nachgebildet");
        }
    }
    if (!(mask & GL_COLOR_BUFFER_BIT)) return;
    if (srcX0 == srcX1 || srcY0 == srcY1 || dstX0 == dstX1 || dstY0 == dstY1) return;
    if (!ensureBlitProgram()) return;

    GLuint* slots = currentRenderer->clientState.framebuffer;
    GLuint readId = slots[indexOfGLTarget(GL_READ_FRAMEBUFFER)];
    GLFramebuffer* readFramebuffer = SparseArray_get(currentRenderer->clientState.framebuffers, readId);
    if (!readFramebuffer) return;
    FBAttachmentInfo* source = &readFramebuffer->colorAttachment[0];

    /* Quelle: 2D-Textur mit bekannter Größe direkt, sonst Kopie des Quellbereichs in eine Hilfstextur */
    int srcLeft = MIN(srcX0, srcX1), srcBottom = MIN(srcY0, srcY1);
    int srcWidth = abs(srcX1 - srcX0), srcHeight = abs(srcY1 - srcY0);
    GLuint sourceTexture = 0;
    int sourceWidth = 0, sourceHeight = 0;
    if (source->type == GL_TEXTURE_2D && source->id != 0) {
        if (source->texture && source->texture->width > 0) {
            sourceTexture = source->id;
            sourceWidth = MAX(1, source->texture->width >> source->level);
            sourceHeight = MAX(1, source->texture->height >> source->level);
        }
        else if (readId == currentRenderer->displayBuffer) {
            sourceTexture = source->id;
            sourceWidth = currentRenderer->displaySize[0];
            sourceHeight = currentRenderer->displaySize[1];
        }
    }
    float u0, u1, v0, v1;
    GLint savedActiveTexture = GL_TEXTURE0, savedTexture2D = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTexture2D);
    if (sourceTexture) {
        u0 = (float)srcLeft / sourceWidth; u1 = (float)(srcLeft + srcWidth) / sourceWidth;
        v0 = (float)srcBottom / sourceHeight; v1 = (float)(srcBottom + srcHeight) / sourceHeight;
    }
    else {
        bool rebound = GLFramebuffer_beginReadAccess();
        if (!blitTempTexture) glGenTextures(1, &blitTempTexture);
        glBindTexture(GL_TEXTURE_2D, blitTempTexture);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, srcLeft, srcBottom, srcWidth, srcHeight, 0);
        GLFramebuffer_endReadAccess(rebound);
        sourceTexture = blitTempTexture;
        u0 = 0.0f; u1 = 1.0f; v0 = 0.0f; v1 = 1.0f;
    }
    if ((srcX1 < srcX0) != (dstX1 < dstX0)) { float t = u0; u0 = u1; u1 = t; }
    if ((srcY1 < srcY0) != (dstY1 < dstY0)) { float t = v0; v0 = v1; v1 = t; }

    /* Zustand sichern */
    GLint savedProgram = 0, savedArrayBuffer = 0, savedViewport[4], savedMinFilter = GL_LINEAR, savedMagFilter = GL_LINEAR, savedWrapS = GL_REPEAT, savedWrapT = GL_REPEAT;
    glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &savedArrayBuffer);
    glGetIntegerv(GL_VIEWPORT, savedViewport);
    GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST), stencilTest = glIsEnabled(GL_STENCIL_TEST), blend = glIsEnabled(GL_BLEND), cullFace = glIsEnabled(GL_CULL_FACE);
    GLboolean sampleCoverage = glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE), polygonOffset = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    SavedAttrib savedAttribs[2];
    saveAttrib(0, &savedAttribs[0]);
    saveAttrib(1, &savedAttribs[1]);

    glBindTexture(GL_TEXTURE_2D, sourceTexture);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &savedMinFilter);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &savedMagFilter);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &savedWrapS);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &savedWrapT);
    GLenum texFilter = filter == GL_LINEAR ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, texFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, texFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* Zeichnen: Viewport = Zielrechteck, Viereck über den ganzen Viewport (Scissor gilt wie beim echten Blit) */
    glUseProgram(blitProgram);
    glUniform1i(blitSamplerLocation, 0);
    glViewport(MIN(dstX0, dstX1), MIN(dstY0, dstY1), abs(dstX1 - dstX0), abs(dstY1 - dstY0));
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const float positions[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    const float texCoords[8] = {u0, v0, u1, v0, u0, v1, u1, v1};
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, positions);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    /* Zustand zurück */
    restoreAttrib(0, &savedAttribs[0]);
    restoreAttrib(1, &savedAttribs[1]);
    glBindBuffer(GL_ARRAY_BUFFER, savedArrayBuffer);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, savedMinFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, savedMagFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, savedWrapS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, savedWrapT);
    glBindTexture(GL_TEXTURE_2D, savedTexture2D);
    glActiveTexture(savedActiveTexture);
    glUseProgram(savedProgram);
    glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
    if (depthTest) glEnable(GL_DEPTH_TEST);
    if (stencilTest) glEnable(GL_STENCIL_TEST);
    if (blend) glEnable(GL_BLEND);
    if (cullFace) glEnable(GL_CULL_FACE);
    if (sampleCoverage) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    if (polygonOffset) glEnable(GL_POLYGON_OFFSET_FILL);
    applyColorMaskES2();
}
