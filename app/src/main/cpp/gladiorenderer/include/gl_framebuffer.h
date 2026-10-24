#ifndef GLADIO_GL_FRAMEBUFFER_H
#define GLADIO_GL_FRAMEBUFFER_H

#include "gladio.h"

#define FLAG_READ_BUFFER_NONE (1<<0)
#define FLAG_READ_BUFFER_COLOR_ATTACHMENT (1<<1)
#define FLAG_DRAW_BUFFER_NONE (1<<9)
#define FLAG_DRAW_BUFFER_COLOR_ATTACHMENT (1<<10)

#define PREFERRED_FRAMEBUFFER_FORMAT GL_RGBA8
#define PREFERRED_RENDERBUFFER_FORMAT GL_DEPTH24_STENCIL8

struct GLTexture;

typedef struct FBAttachmentInfo {
    GLenum type;
    GLuint id;
    uint8_t level;
    struct GLTexture* texture; /* Texturobjekt des Clients, falls bekannt (für die Blit-Nachbildung in ES 2.0) */
} FBAttachmentInfo;

typedef struct GLFramebuffer {
    GLuint ownerId;
    GLuint id;
    FBAttachmentInfo colorAttachment[MAX_FB_COLOR_ATTACHMENTS];
    FBAttachmentInfo depthAttachment;
    FBAttachmentInfo stencilAttachment;
    GLuint flags;
    /* ES 2.0: eigener Tiefen-/Stencil-Renderbuffer, wenn der angehängte nicht die Größe des
     * Farbanhangs hat (ES 2.0 verlangt gleich große Anhänge) */
    GLuint substituteDepth;
    int substituteWidth, substituteHeight;
    bool substituteActive;
    bool substituteFresh;
} GLFramebuffer;

extern GLuint GLFramebuffer_create();
extern void GLFramebuffer_bind(GLenum target, GLuint id);
extern GLFramebuffer* GLFramebuffer_getBound(GLenum target);
extern void GLFramebuffer_setAttachment(GLenum target, GLenum attachment, GLenum objectType, GLuint objectId, uint8_t level, struct GLTexture* texture);
extern void GLFramebuffer_delete(GLuint id);
extern void GLFramebuffer_setReadBuffer(GLenum src);
extern void GLFramebuffer_setDrawBuffers(GLuint count, GLenum* dst);
extern void GLFramebuffer_getParamsv(GLenum target, GLenum attachment, GLenum pname, GLint *params);
extern GLenum GLFramebuffer_checkStatus(GLenum target);
/* glClear; in ES 2.0 werden Tiefen-/Stencil-Löschungen an die Ersatzpuffer weitergereicht. */
extern void GLFramebuffer_clear(GLbitfield mask);
/* Größe eines Renderbuffers merken (für den Größenabgleich der Anhänge in ES 2.0). */
extern void GLFramebuffer_noteRenderbufferSize(GLuint renderbuffer, int width, int height);

/* ES 2.0 kennt nur einen Bindungspunkt. Gebunden ist immer das Zeichen-FBO; das Lese-FBO wird
 * getrennt geführt und für Leseoperationen (glReadPixels, glCopyTex*Image2D, Anfragen an das
 * Lese-FBO) vorübergehend gebunden: begin liefert true, wenn umgebunden wurde, end bindet zurück. */
extern bool GLFramebuffer_beginReadAccess(void);
extern void GLFramebuffer_endReadAccess(bool rebound);
/* Farbmaske der Anwendung; in ES 2.0 zusammen mit glDrawBuffer(GL_NONE) angewendet. */
extern void GLFramebuffer_setColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
/* glBlitFramebuffer in ES 2.0: Farbe als texturiertes Viereck vom Lese- ins Zeichen-FBO. */
extern void GLFramebuffer_blit(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);

#endif