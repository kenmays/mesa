/*
 * Copyright 2012-2014, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *      Artur Wyszynski, harakash@gmail.com
 *      Alexander von Gluck IV, kallisti5@unixzen.com
 */

#include "hgl_context.h"

#include <stdio.h>

#include "util/format/u_formats.h"
#include "util/u_atomic.h"
#include "util/format/u_format.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "state_tracker/st_context.h"


#if MESA_DEBUG
#   define TRACE(x...) printf("hgl:frontend: " x)
#   define CALLED() TRACE("CALLED: %s\n", __PRETTY_FUNCTION__)
#else
#   define TRACE(x...)
#   define CALLED()
#endif
#define ERROR(x...) printf("hgl:frontend: " x)


// Perform a safe void to hgl_context cast
static inline struct hgl_context *
hgl_st_context(struct st_context *st)
{
    struct hgl_context *context;

    assert(st);
    context = (struct hgl_context *)st->frontend_context;
    assert(context);
    return context;
}


// Perform a safe void to hgl_buffer cast
static struct hgl_buffer *
hgl_st_framebuffer(struct pipe_frontend_drawable *drawable)
{
    if (drawable == NULL)
        return NULL;

    return (struct hgl_buffer *)drawable;
}


static bool
hgl_st_framebuffer_flush_front(struct st_context *st,
    struct pipe_frontend_drawable *drawable, enum st_attachment_type statt)
{
    struct hgl_buffer *buffer = hgl_st_framebuffer(drawable);
    struct pipe_resource *resource = NULL;

    CALLED();

    if (buffer == NULL || statt != ST_ATTACHMENT_FRONT_LEFT)
        return false;

    /* Pbuffers have no native window to present to. */
    if (buffer->winsysContext == NULL)
        return true;

    if (!buffer->mutex_initialized)
        return false;

    mtx_lock(&buffer->mutex);
    pipe_resource_reference(&resource,
        buffer->textures[ST_ATTACHMENT_FRONT_LEFT]);
    mtx_unlock(&buffer->mutex);

    if (resource == NULL)
        return true;

    if (buffer->screen->flush_frontbuffer != NULL) {
        buffer->screen->flush_frontbuffer(buffer->screen, NULL, resource,
            0, 0, buffer->winsysContext, 0, NULL);
    }

    pipe_resource_reference(&resource, NULL);
    return true;
}


/*
 * Allocate the complete requested attachment set before replacing any of the
 * framebuffer's current resources. A failed allocation leaves the old set
 * intact, including its dimensions and mask.
 */
static bool
hgl_st_framebuffer_validate(struct st_context *st,
    struct pipe_frontend_drawable *drawable,
    const enum st_attachment_type *statts, unsigned count,
    struct pipe_resource **out, struct pipe_resource **resolve)
{
    struct hgl_buffer *buffer = hgl_st_framebuffer(drawable);
    struct pipe_resource *replacement[ST_ATTACHMENT_COUNT] = { NULL };
    struct pipe_resource templ;
    unsigned requested_mask = 0;
    unsigned width, height;
    unsigned i;
    bool resized;
    bool success = false;

    CALLED();

    if (buffer == NULL || !buffer->mutex_initialized || out == NULL
        || (count != 0 && statts == NULL) || count > ST_ATTACHMENT_COUNT)
        return false;

    if (resolve != NULL)
        *resolve = NULL;

    for (i = 0; i < count; i++) {
        unsigned attachment = (unsigned)statts[i];

        if (attachment >= ST_ATTACHMENT_COUNT)
            return false;

        if (!(buffer->visual.buffer_mask & (1u << attachment)))
            return false;

        requested_mask |= 1u << attachment;
    }

    mtx_lock(&buffer->mutex);

    width = buffer->newWidth;
    height = buffer->newHeight;
    resized = buffer->width != width || buffer->height != height;

    if (width == 0 || height == 0)
        goto done;

    memset(&templ, 0, sizeof(templ));
    templ.target = buffer->target;
    templ.width0 = width;
    templ.height0 = height;
    templ.depth0 = 1;
    templ.array_size = 1;
    templ.last_level = 0;
    templ.nr_samples = buffer->visual.samples;
    templ.nr_storage_samples = buffer->visual.samples;

    for (i = 0; i < ST_ATTACHMENT_COUNT; i++) {
        enum pipe_format format = PIPE_FORMAT_NONE;
        unsigned bind = 0;

        if (!(requested_mask & (1u << i)))
            continue;

        switch (i) {
        case ST_ATTACHMENT_FRONT_LEFT:
        case ST_ATTACHMENT_BACK_LEFT:
        case ST_ATTACHMENT_FRONT_RIGHT:
        case ST_ATTACHMENT_BACK_RIGHT:
            format = buffer->visual.color_format;
            bind = PIPE_BIND_RENDER_TARGET;
            if (buffer->winsysContext != NULL)
                bind |= PIPE_BIND_DISPLAY_TARGET;
            break;

        case ST_ATTACHMENT_DEPTH_STENCIL:
            format = buffer->visual.depth_stencil_format;
            bind = PIPE_BIND_DEPTH_STENCIL;
            break;

        default:
            /* Accumulation and other attachments are not implemented. */
            goto done;
        }

        if (format == PIPE_FORMAT_NONE)
            goto done;

        if (!resized && buffer->textures[i] != NULL) {
            pipe_resource_reference(&replacement[i], buffer->textures[i]);
            continue;
        }

        templ.format = format;
        templ.bind = bind;
        replacement[i] = buffer->screen->resource_create(
            buffer->screen, &templ);

        if (replacement[i] == NULL)
            goto done;
    }

    /*
     * A native view may still reference the current front/back bitmap even
     * when the state tracker no longer requests that attachment. Detach it
     * before dropping or replacing any native display-target resource.
     */
    if (buffer->winsysContext != NULL) {
        bool native_target_changed = false;

        for (i = ST_ATTACHMENT_FRONT_LEFT; i <= ST_ATTACHMENT_BACK_RIGHT; i++) {
            if (buffer->textures[i] != replacement[i]) {
                native_target_changed = true;
                break;
            }
        }

        if (native_target_changed)
            hgl_winsys_detach_native_surface(buffer->winsysContext);
    }

    /* Commit the new attachment set only after every allocation succeeded. */
    for (i = 0; i < ST_ATTACHMENT_COUNT; i++) {
        pipe_resource_reference(&buffer->textures[i], replacement[i]);
        pipe_resource_reference(&replacement[i], NULL);
    }

    buffer->width = width;
    buffer->height = height;
    buffer->mask = requested_mask;

    for (i = 0; i < count; i++) {
        unsigned attachment = (unsigned)statts[i];
        pipe_resource_reference(&out[i], buffer->textures[attachment]);
    }

    success = true;

done:
    for (i = 0; i < ST_ATTACHMENT_COUNT; i++)
        pipe_resource_reference(&replacement[i], NULL);

    mtx_unlock(&buffer->mutex);
    return success;
}


static uint32_t hgl_fb_ID = 0;


/**
 * Create new framebuffer.
 */
struct hgl_buffer *
hgl_create_st_framebuffer(struct hgl_display *display, struct st_visual *visual,
    void *winsysContext)
{
    struct hgl_buffer *buffer;

    CALLED();

    if (display == NULL || display->fscreen == NULL
        || display->fscreen->screen == NULL || visual == NULL)
        return NULL;

    buffer = CALLOC_STRUCT(hgl_buffer);
    if (buffer == NULL)
        return NULL;

    if (mtx_init(&buffer->mutex, mtx_plain) != thrd_success) {
        FREE(buffer);
        return NULL;
    }
    buffer->mutex_initialized = true;

    buffer->visual = *visual;
    buffer->screen = display->fscreen->screen;
    buffer->winsysContext = winsysContext;

    if (buffer->screen->caps.npot_textures)
        buffer->target = PIPE_TEXTURE_2D;
    else
        buffer->target = PIPE_TEXTURE_RECT;

    buffer->base.flush_front = hgl_st_framebuffer_flush_front;
    buffer->base.validate = hgl_st_framebuffer_validate;
    buffer->base.visual = &buffer->visual;

    p_atomic_set(&buffer->base.stamp, 1);
    buffer->base.ID = p_atomic_inc_return(&hgl_fb_ID);
    buffer->base.fscreen = display->fscreen;

    return buffer;
}


void
hgl_destroy_st_framebuffer(struct hgl_buffer *buffer)
{
    unsigned i;

    CALLED();

    if (buffer == NULL)
        return;

    if (buffer->mutex_initialized) {
        mtx_lock(&buffer->mutex);
        for (i = 0; i < ST_ATTACHMENT_COUNT; i++)
            pipe_resource_reference(&buffer->textures[i], NULL);
        mtx_unlock(&buffer->mutex);
        mtx_destroy(&buffer->mutex);
        buffer->mutex_initialized = false;
    }

    FREE(buffer);
}


struct hgl_context *
hgl_create_context_with_attribs(struct hgl_display *display,
    struct st_visual *visual, struct st_context *shared,
    const struct hgl_context_attribs *context_attribs)
{
    struct hgl_context *context;
    struct st_context_attribs attribs;
    enum st_context_error result;

    if (display == NULL || display->fscreen == NULL
        || display->fscreen->screen == NULL || visual == NULL)
        return NULL;

    context = CALLOC_STRUCT(hgl_context);
    if (context == NULL)
        return NULL;

    context->display = display;

    memset(&attribs, 0, sizeof(attribs));
    attribs.options.force_glsl_extensions_warn = false;
    attribs.profile = context_attribs != NULL
        ? (gl_api)context_attribs->profile : API_OPENGL_COMPAT;
    attribs.visual = *visual;
    attribs.major = context_attribs != NULL && context_attribs->major != 0
        ? context_attribs->major : 1;
    attribs.minor = context_attribs != NULL
        ? context_attribs->minor : 0;
    attribs.flags = context_attribs != NULL ? context_attribs->flags : 0;
    attribs.context_flags = context_attribs != NULL
        ? context_attribs->context_flags : 0;

    context->st = st_api_create_context(display->fscreen, &attribs,
        &result, shared);
    if (context->st == NULL) {
        FREE(context);
        return NULL;
    }

    assert(!context->st->frontend_context);
    context->st->frontend_context = (void *)context;

    return context;
}


struct hgl_context *
hgl_create_context(struct hgl_display *display, struct st_visual *visual,
    struct st_context *shared)
{
    return hgl_create_context_with_attribs(display, visual, shared, NULL);
}


void
hgl_destroy_context(struct hgl_context *context)
{
    if (context == NULL)
        return;

    if (context->st != NULL) {
        st_context_flush(context->st, 0, NULL, NULL, NULL);
        st_destroy_context(context->st);
    }

    FREE(context);
}


void
hgl_get_st_visual(struct st_visual *visual, ulong options)
{
    CALLED();

    if (visual == NULL)
        return;

    memset(visual, 0, sizeof(*visual));

    if ((options & HGL_INDEX) != 0) {
        /*
         * This is a 16-bit RGB visual, not a palette-indexed visual.
         * True indexed-color support is not implemented by this frontend.
         */
        visual->color_format = PIPE_FORMAT_B5G6R5_UNORM;
        visual->depth_stencil_format = PIPE_FORMAT_NONE;
    } else {
        visual->color_format = (options & HGL_ALPHA)
            ? PIPE_FORMAT_BGRA8888_UNORM : PIPE_FORMAT_BGRX8888_UNORM;

        if (options & HGL_DEPTH) {
            visual->depth_stencil_format = (options & HGL_STENCIL)
                ? PIPE_FORMAT_Z24_UNORM_S8_UINT : PIPE_FORMAT_Z24X8_UNORM;
        } else if (options & HGL_STENCIL) {
            visual->depth_stencil_format = PIPE_FORMAT_X24S8_UINT;
        } else {
            visual->depth_stencil_format = PIPE_FORMAT_NONE;
        }
    }

    /* Accumulation buffers are not allocated by this frontend. */
    visual->accum_format = PIPE_FORMAT_NONE;

    visual->buffer_mask = ST_ATTACHMENT_FRONT_LEFT_MASK;
    if (options & HGL_DOUBLE)
        visual->buffer_mask |= ST_ATTACHMENT_BACK_LEFT_MASK;

#if 0
    if (options & HGL_STEREO) {
        visual->buffer_mask |= ST_ATTACHMENT_FRONT_RIGHT_MASK;
        if (options & HGL_DOUBLE)
            visual->buffer_mask |= ST_ATTACHMENT_BACK_RIGHT_MASK;
    }
#endif

    if (options & (HGL_DEPTH | HGL_STENCIL))
        visual->buffer_mask |= ST_ATTACHMENT_DEPTH_STENCIL_MASK;

    TRACE("%s: Visual color format: %s\n", __func__,
        util_format_name(visual->color_format));
}


struct hgl_display *
hgl_create_display(struct pipe_screen *screen)
{
    struct hgl_display *display;

    if (screen == NULL)
        return NULL;

    display = CALLOC_STRUCT(hgl_display);
    if (display == NULL)
        return NULL;

    if (mtx_init(&display->mutex, mtx_plain) != thrd_success) {
        FREE(display);
        return NULL;
    }

    display->fscreen = CALLOC_STRUCT(pipe_frontend_screen);
    if (display->fscreen == NULL) {
        mtx_destroy(&display->mutex);
        FREE(display);
        return NULL;
    }

    display->fscreen->screen = screen;
    display->fscreen->broken_invalidate = true;

    return display;
}


void
hgl_destroy_display(struct hgl_display *display)
{
    if (display == NULL)
        return;

    if (display->fscreen != NULL) {
        st_screen_destroy(display->fscreen);
        FREE(display->fscreen);
    }

    mtx_destroy(&display->mutex);
    FREE(display);
}
