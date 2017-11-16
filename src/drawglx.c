/*
 * drawglx.c
 *
 *  Created on: Nov 9, 2017
 *      Author: nullifiedcat
 */

#include "drawglx_internal.h"
#include "overlay.h"
#include "program.h"
#include "vertex_structs.h"
#include "log.h"

#include <GL/gl.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xfixes.h>

#define GLX_CONTEXT_MAJOR_VERSION_ARB           0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB           0x2092
typedef GLXContext (*glXCreateContextAttribsARBfn)(Display *, GLXFBConfig, GLXContext, Bool, const int *);

// Helper to check for extension string presence.  Adapted from:
//   http://www.opengl.org/resources/features/OGLextensions/
int glx_is_extension_supported(const char *list, const char *extension)
{
    const char *start;
    const char *where, *terminator;

    where = strchr(extension, ' ');
    if (where || *extension == '\0')
        return 0;

    start = list;
    while (1)
    {
        where = strstr(start, extension);

        if (!where)
        break;

        terminator = where + strlen(extension);

        if ( where == start || *(where - 1) == ' ' )
            if ( *terminator == ' ' || *terminator == '\0' )
                return 1;

        start = terminator;
    }

    return 0;
}

int xoverlay_glx_init()
{
    glXQueryVersion(xoverlay_library.display, &glx_state.version_major, &glx_state.version_minor);
    log_write("GL Version: %s\n", glGetString(GL_VERSION));
    return 0;
}

int xoverlay_glx_create_window()
{
    GLint attribs[] = {
            GLX_X_RENDERABLE, GL_TRUE,
            GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
            GLX_RENDER_TYPE, GLX_RGBA_BIT,
            GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
            GLX_DEPTH_SIZE, 24,
            GLX_STENCIL_SIZE, 8,
            GLX_RED_SIZE, 8,
            GLX_GREEN_SIZE, 8,
            GLX_BLUE_SIZE, 8,
            GLX_ALPHA_SIZE, 8,
            GLX_DOUBLEBUFFER, GL_TRUE,
            None
    };

    int fbc_count;
    GLXFBConfig *fbc = glXChooseFBConfig(xoverlay_library.display, xoverlay_library.screen, attribs, &fbc_count);
    if (fbc == NULL)
    {
        return -1;
    }
    int fbc_best = -1;
    int fbc_best_samples = -1;
    for (int i = 0; i < fbc_count; ++i)
    {
        XVisualInfo *info = glXGetVisualFromFBConfig(xoverlay_library.display, fbc[i]);
        if (info->depth != 32)
            continue;
        int samples;
        glXGetFBConfigAttrib(xoverlay_library.display, fbc[i], GLX_SAMPLES, &samples);
        if (fbc_best < 0 || samples > fbc_best_samples)
        {
            fbc_best = i;
            fbc_best_samples = samples;
        }
        XFree(info);
    }
    if (fbc_best == -1)
    {
        log_write("Could not get FB config with 32 depth\n");
        return -1;
    }
    GLXFBConfig fbconfig = fbc[fbc_best];
    XFree(fbc);

    XVisualInfo *info = glXGetVisualFromFBConfig(xoverlay_library.display, fbconfig);
    if (info == NULL)
    {
        log_write("GLX initialization error\n");
        return -1;
    }
    Window root = DefaultRootWindow(xoverlay_library.display);
    xoverlay_library.colormap = XCreateColormap(xoverlay_library.display, root, info->visual, AllocNone);
    XSetWindowAttributes attr;
    attr.background_pixel = 0x0;
    attr.border_pixel = 0;
    attr.save_under = 1;
    attr.override_redirect = 1;
    attr.colormap = xoverlay_library.colormap;
    attr.event_mask = (StructureNotifyMask|ExposureMask|PropertyChangeMask|EnterWindowMask|LeaveWindowMask|KeyPressMask|KeyReleaseMask|KeymapStateMask);
    attr.do_not_propagate_mask = (KeyPressMask|KeyReleaseMask|ButtonPressMask|ButtonReleaseMask|PointerMotionMask|ButtonMotionMask);

    unsigned long mask = CWBackPixel | CWBorderPixel | CWSaveUnder | CWOverrideRedirect | CWColormap | CWEventMask | CWDontPropagate;
    log_write("depth %d\n", info->depth);
    xoverlay_library.window = XCreateWindow(xoverlay_library.display, root, 0, 0, xoverlay_library.width, xoverlay_library.height, 0, info->depth, InputOutput, info->visual, mask, &attr);
    if (xoverlay_library.window == 0)
    {
        log_write("X window initialization error\n");
        return -1;
    }

    XShapeCombineMask(xoverlay_library.display, xoverlay_library.window, ShapeInput, 0, 0, None, ShapeSet);
    XShapeSelectInput(xoverlay_library.display, xoverlay_library.window, ShapeNotifyMask);

    XserverRegion region = XFixesCreateRegion(xoverlay_library.display, NULL, 0);
    XFixesSetWindowShapeRegion(xoverlay_library.display, xoverlay_library.window, ShapeInput, 0, 0, region);
    XFixesDestroyRegion(xoverlay_library.display, region);

    XFree(info);
    XStoreName(xoverlay_library.display, xoverlay_library.window, "OverlayWindow");

    xoverlay_show();

    const char *extensions = glXQueryExtensionsString(xoverlay_library.display, xoverlay_library.screen);
    glXCreateContextAttribsARBfn glXCreateContextAttribsARB = (glXCreateContextAttribsARBfn)
            glXGetProcAddressARB((const GLubyte *) "glXCreateContextAttribsARB");

    if (!glx_is_extension_supported(extensions, "GLX_ARB_create_context"))
    {
        log_write("Falling back to glXCreateNewContext\n");
        glx_state.context = glXCreateNewContext(xoverlay_library.display, fbconfig, GLX_RGBA_TYPE, NULL, GL_TRUE);
    }
    else
    {
        int ctx_attribs[] = {
                GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
                GLX_CONTEXT_MINOR_VERSION_ARB, 0,
                None
        };
        glx_state.context = glXCreateContextAttribsARB(xoverlay_library.display, fbconfig, NULL, GL_TRUE, ctx_attribs);
        XSync(xoverlay_library.display, GL_FALSE);
    }
    if (glx_state.context == NULL)
    {
        log_write("OpenGL context initialization error\n");
        return -1;
    }
    if (!glXIsDirect(xoverlay_library.display, glx_state.context))
    {
        log_write("Context is indirect\n");
    }
    else
    {
        log_write("Context is direct\n");
    }
    glXMakeCurrent(xoverlay_library.display, xoverlay_library.window, glx_state.context);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK)
    {
        log_write("GLEW initialization error: %s\n", glewGetErrorString(glGetError()));
        return -1;
    }
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glXSwapBuffers(xoverlay_library.display, xoverlay_library.window);

    log_write("Initializing DS\n");
    ds_init();
    program_init();

    return 0;
}

void
xoverlay_draw_circle(float x, float y, float radius, xoverlay_rgba_t color, float thickness, int steps)
{
    float px = 0;
    float py = 0;
    for (int i = 0; i < steps; i++) {
            float ang = 2 * M_PI * ((float)i / steps);
            if (!i)
                ang = 2 * M_PI * ((float)(steps - 1) / steps);
            if (i)
                xoverlay_draw_line(px, py, x - px + radius * cos(ang), y - py + radius * sin(ang), color, thickness);
            px = x + radius * cos(ang);
            py = y + radius * sin(ang);
    }
}

void
xoverlay_draw_line(float x, float y, float dx, float dy, xoverlay_rgba_t color, float thickness)
{
    if (xoverlay_library.mapped == 0 || xoverlay_library.drawing == 0)
        return;

    x += 0.5f;
    y += 0.5f;
    dx -= 0.5f;
    dy -= 0.5f;

    GLuint indices[6] = { 0, 1, 3, 3, 2, 0 };

    struct vertex_main vertices[4];

    float nx = -dy;
    float ny = dx;

    float ex = x + dx;
    float ey = y + dy;

    float length = sqrtf(nx * nx + ny * ny);

    if (length == 0)
        return;

    length /= thickness;
    nx /= length;
    ny /= length;

    vertices[0].position.x = x - nx;
    vertices[0].position.y = y - ny;
    vertices[0].color = *(vec4*)&color;
    vertices[0].mode = DRAW_MODE_PLAIN;

    vertices[1].position.x = x + nx;
    vertices[1].position.y = y + ny;
    vertices[1].color = *(vec4*)&color;
    vertices[1].mode = DRAW_MODE_PLAIN;


    vertices[2].position.x = ex - nx;
    vertices[2].position.y = ey - ny;
    vertices[2].color = *(vec4*)&color;
    vertices[2].mode = DRAW_MODE_PLAIN;


    vertices[3].position.x = ex + nx;
    vertices[3].position.y = ey + ny;
    vertices[3].color = *(vec4*)&color;
    vertices[3].mode = DRAW_MODE_PLAIN;

    vertex_buffer_push_back(program.buffer, vertices, 4, indices, 6);
}

void
xoverlay_draw_rect(float x, float y, float w, float h, xoverlay_rgba_t color)
{
    if (xoverlay_library.mapped == 0 || xoverlay_library.drawing == 0)
        return;

    x += 0.5f;
    y += 0.5f;
    w -= 0.5f;
    h -= 0.5f;

    struct vertex_main vertices[4];
    GLuint indices[6] = { 0, 1, 2, 2, 3, 0 };

    vertices[0].position.x = x;
    vertices[0].position.y = y;
    vertices[0].color = *(vec4*)&color;
    vertices[0].mode = DRAW_MODE_PLAIN;

    vertices[1].position.x = x;
    vertices[1].position.y = y + h;
    vertices[1].color = *(vec4*)&color;
    vertices[1].mode = DRAW_MODE_PLAIN;

    vertices[2].position.x = x + w;
    vertices[2].position.y = y + h;
    vertices[2].color = *(vec4*)&color;
    vertices[2].mode = DRAW_MODE_PLAIN;

    vertices[3].position.x = x + w;
    vertices[3].position.y = y;
    vertices[3].color = *(vec4*)&color;
    vertices[3].mode = DRAW_MODE_PLAIN;

    vertex_buffer_push_back(program.buffer, vertices, 4, indices, 6);
}

void
xoverlay_draw_rect_outline(float x, float y, float w, float h, xoverlay_rgba_t color, float thickness)
{
    if (xoverlay_library.mapped == 0 || xoverlay_library.drawing == 0)
        return;

    xoverlay_draw_line(x, y, w, 0, color, thickness);
    xoverlay_draw_line(x + w, y, 0, h, color, thickness);
    xoverlay_draw_line(x + w, y + h, -w, 0, color, thickness);
    xoverlay_draw_line(x, y + h, 0, -h, color, thickness);
}

void
xoverlay_draw_rect_textured(float x, float y, float w, float h, xoverlay_rgba_t color, xoverlay_texture_handle_t texture, float tx, float ty, float tw, float th)
{
    if (xoverlay_library.mapped == 0 || xoverlay_library.drawing == 0)
        return;

    struct textureapi_texture_t *tex = textureapi_get(texture);

    if (tex == NULL)
        return;

    textureapi_bind(texture);

    x += 0.5f;
    y += 0.5f;
    w -= 0.5f;
    h -= 0.5f;

    GLuint idx = program_next_index();

    struct vertex_main vertices[4];
    GLuint indices[6] = { idx, idx + 1, idx + 2, idx + 2, idx + 3, idx };

    float s0 = tx / tex->width;
    float s1 = (tx + tw) / tex->width;
    float t0 = ty / tex->height;
    float t1 = (ty + th) / tex->height;

    vertices[0].position.x = x;
    vertices[0].position.y = y;
    vertices[0].tex_coords.x = s0;
    vertices[0].tex_coords.y = t1;
    vertices[0].color = *(vec4*)&color;
    vertices[0].mode = DRAW_MODE_TEXTURED;

    vertices[1].position.x = x;
    vertices[1].position.y = y + h;
    vertices[1].tex_coords.x = s0;
    vertices[1].tex_coords.y = t0;
    vertices[1].color = *(vec4*)&color;
    vertices[1].mode = DRAW_MODE_TEXTURED;

    vertices[2].position.x = x + w;
    vertices[2].position.y = y + h;
    vertices[2].tex_coords.x = s1;
    vertices[2].tex_coords.y = t0;
    vertices[2].color = *(vec4*)&color;
    vertices[2].mode = DRAW_MODE_TEXTURED;

    vertices[3].position.x = x + w;
    vertices[3].position.y = y;
    vertices[3].tex_coords.x = s1;
    vertices[3].tex_coords.y = t1;
    vertices[3].color = *(vec4*)&color;
    vertices[3].mode = DRAW_MODE_TEXTURED;

    vertex_buffer_push_back(program.buffer, vertices, 4, indices, 6);
}

void
draw_string_internal(float x, float y, const char *string, texture_font_t *fnt, vec4 color, float *out_x, float *out_y)
{
    float pen_x = x;
    float pen_y = y + fnt->height / 1.5f;
    float size_y = 0;

    //texture_font_load_glyphs(fnt, string);

    if (fnt->atlas->id == 0)
    {
        glGenTextures(1, &fnt->atlas->id);
    }
    ds_bind_texture(fnt->atlas->id);
    if (fnt->atlas->dirty) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, fnt->atlas->width, fnt->atlas->height, 0, GL_RED, GL_UNSIGNED_BYTE, fnt->atlas->data);
        fnt->atlas->dirty = 0;
    }

    int len = strlen(string);
    if (len == 0)
        return;

    for (size_t i = 0; i < len; ++i)
    {
        texture_glyph_t *glyph = texture_font_find_glyph(fnt, &string[i]);
        if (glyph == NULL)
        {
            texture_font_load_glyph(fnt, &string[i]);
            continue;
        }
        GLuint indices[6];
        struct vertex_main vertices[4];
        if (i > 0)
        {
            x += texture_glyph_get_kerning(glyph, &string[i - 1]);
        }

        float x0 = (pen_x + glyph->offset_x);
        float y0 = (pen_y - glyph->offset_y);
        float x1 = (x0 + glyph->width);
        float y1 = (y0 + glyph->height);
        float s0 = glyph->s0;
        float t0 = glyph->t0;
        float s1 = glyph->s1;
        float t1 = glyph->t1;

        indices[0] = 0;
        indices[1] = 1;
        indices[2] = 2;
        indices[3] = 2;
        indices[4] = 3;
        indices[5] = 0;

        vertices[0] = (struct vertex_main){ (vec2){ x0, y0 }, (vec2){ s0, t0 }, color, DRAW_MODE_FREETYPE };
        vertices[1] = (struct vertex_main){ (vec2){ x0, y1 }, (vec2){ s0, t1 }, color, DRAW_MODE_FREETYPE };
        vertices[2] = (struct vertex_main){ (vec2){ x1, y1 }, (vec2){ s1, t1 }, color, DRAW_MODE_FREETYPE };
        vertices[3] = (struct vertex_main){ (vec2){ x1, y0 }, (vec2){ s1, t0 }, color, DRAW_MODE_FREETYPE };

        pen_x += glyph->advance_x;
        pen_x = (int)pen_x + 1;
        if (glyph->height > size_y)
            size_y = glyph->height;

        vertex_buffer_push_back(program.buffer, vertices, 4, indices, 6);
    }

    if (out_x)
        *out_x = pen_x - x;
    if (out_y)
        *out_y = size_y;
}
void
xoverlay_draw_string(float x, float y, const char *string, xoverlay_font_handle_t font, xoverlay_vec4_t color, float *out_x, float *out_y)
{
    if (xoverlay_library.mapped == 0 || xoverlay_library.drawing == 0)
        return;

    texture_font_t *fnt = fontapi_get(font);
    if (fnt == NULL)
    {
        log_write("xoverlay_draw_string: INVALID FONT HANDLE %u\n", font);
        return;
    }

    ds_bind_texture(fnt->atlas->id);

    fnt->rendermode = RENDER_NORMAL;
    fnt->outline_thickness = 0.0f;

    draw_string_internal(x, y, string, fnt, *(vec4*)&color, out_x, out_y);
}

void
xoverlay_draw_string_with_outline(float x, float y, const char *string, xoverlay_font_handle_t font, xoverlay_vec4_t color, xoverlay_vec4_t outline_color, float outline_width, int adjust_outline_alpha, float *out_x, float *out_y)
{
    if (xoverlay_library.mapped == 0 || xoverlay_library.drawing == 0)
        return;

    if (adjust_outline_alpha)
        outline_color.a = color.a;

    texture_font_t *fnt = fontapi_get(font);
    if (fnt == NULL)
    {
        log_write("xoverlay_draw_string: INVALID FONT HANDLE %u\n", font);
        return;
    }

    fnt->rendermode = RENDER_OUTLINE_POSITIVE;
    fnt->outline_thickness = outline_width;
    draw_string_internal(x, y, string, fnt, *(vec4*)&outline_color, NULL, NULL);

    fnt->rendermode = RENDER_NORMAL;
    fnt->outline_thickness = 0.0f;
    draw_string_internal(x, y, string, fnt, *(vec4*)&color, out_x, out_y);
}

void
xoverlay_get_string_size(const char *string, xoverlay_font_handle_t font, float *out_x, float *out_y)
{

    float pen_x = 0;
    float pen_y = 0;

    float size_x = 0;
    float size_y = 0;

    texture_font_t *fnt = fontapi_get(font);
    if (fnt == NULL)
        return;
    texture_font_load_glyphs(fnt, string);

    for (size_t i = 0; i < strlen(string); ++i)
    {
        texture_glyph_t *glyph = texture_font_find_glyph(fnt, &string[i]);
        if (glyph == NULL)
        {
            continue;
        }

        pen_x += texture_glyph_get_kerning(glyph, &string[i]);
        pen_x += glyph->advance_x;
        if (pen_x > size_x)
            size_x = pen_x;

        if (glyph->height > size_y)
            size_y = glyph->height;
    }
    if (out_x)
        *out_x = size_x;
    if (out_y)
        *out_y = size_y;
}

int xoverlay_glx_destroy()
{
    return 0;
}
