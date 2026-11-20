/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2014 Adrián Arroyo Calle <adrian.arroyocalle@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <new>

#include <algorithm>

#include <EGL/egl.h>

#include "eglconfig.h"
#include "eglcontext.h"
#include "eglcurrent.h"
#include "egldevice.h"
#include "egldisplay.h"
#include "egldriver.h"
#include "eglimage.h"
#include "egllog.h"
#include "eglsurface.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "state_tracker/st_context.h"
#include "util/u_atomic.h"
#include "mesa/glapi/glapi/glapi.h"

#include "hgl/hgl_sw_winsys.h"
#include "hgl_context.h"

#include <Bitmap.h>

extern "C" {
#include "target-helpers/inline_sw_helper.h"
}

#if MESA_DEBUG
#define TRACE(x...) printf("egl_haiku: " x)
#define CALLED()    TRACE("CALLED: %s\n", __PRETTY_FUNCTION__)
#else
#define TRACE(x...)
#define CALLED()
#endif
#define ERROR(x...) printf("egl_haiku: " x)

_EGL_DRIVER_STANDARD_TYPECASTS(haiku_egl)

class HaikuNativeSurface : public BitmapHook {
public:
   explicit HaikuNativeSurface(BitmapHook *window)
      : fWindow(window), fMutexInitialized(false), fAttachedBitmap(NULL),
        fWidth(0), fHeight(0), fGeneration(0), fLost(false)
   {
      if (fWindow != NULL)
         fMutexInitialized = mtx_init(&fMutex, mtx_plain) == thrd_success;
   }

   ~HaikuNativeSurface() override
   {
      if (fMutexInitialized) {
         mtx_lock(&fMutex);
         DetachLocked();
         mtx_unlock(&fMutex);
         mtx_destroy(&fMutex);
      }
   }

   bool IsValid() const
   {
      return fWindow != NULL && fMutexInitialized;
   }

   void GetSize(uint32_t &width, uint32_t &height) override
   {
      if (!IsValid()) {
         width = 0;
         height = 0;
         return;
      }

      mtx_lock(&fMutex);
      RefreshSizeLocked();
      width = fWidth;
      height = fHeight;
      mtx_unlock(&fMutex);
   }

   BBitmap *SetBitmap(BBitmap *bitmap) override
   {
      BBitmap *previous = NULL;

      if (!IsValid())
         return NULL;

      mtx_lock(&fMutex);
      RefreshSizeLocked();

      if (bitmap == NULL) {
         previous = DetachLocked();
      } else if (!fLost) {
         BRect bounds = bitmap->Bounds();
         uint32_t bitmapWidth = bounds.IntegerWidth() + 1;
         uint32_t bitmapHeight = bounds.IntegerHeight() + 1;

         if (bitmapWidth == fWidth && bitmapHeight == fHeight) {
            if (bitmap == fAttachedBitmap) {
               previous = bitmap;
            } else {
               previous = fWindow->SetBitmap(bitmap);
               fAttachedBitmap = bitmap;
               ++fGeneration;
            }
         }
      }

      mtx_unlock(&fMutex);
      return previous;
   }

private:
   void RefreshSizeLocked()
   {
      uint32_t width = 0;
      uint32_t height = 0;

      fWindow->GetSize(width, height);
      if (width != fWidth || height != fHeight) {
         DetachLocked();
         fWidth = width;
         fHeight = height;
         fLost = width == 0 || height == 0;
         ++fGeneration;
      }
   }

   BBitmap *DetachLocked()
   {
      BBitmap *previous = NULL;

      if (fAttachedBitmap != NULL) {
         previous = fWindow->SetBitmap(NULL);
         fAttachedBitmap = NULL;
         ++fGeneration;
      }

      return previous;
   }

   BitmapHook *fWindow;
   mtx_t fMutex;
   bool fMutexInitialized;
   BBitmap *fAttachedBitmap;
   uint32_t fWidth;
   uint32_t fHeight;
   uint64_t fGeneration;
   bool fLost;
};

static unsigned
haiku_visual_options(const struct _egl_config *conf, bool double_buffer)
{
   unsigned options = double_buffer ? HGL_DOUBLE : HGL_SINGLE;
   if (conf != NULL) {
      if (conf->AlphaSize > 0) options |= HGL_ALPHA;
      if (conf->DepthSize > 0) options |= HGL_DEPTH;
      if (conf->StencilSize > 0) options |= HGL_STENCIL;
   }
   return options;
}

struct haiku_egl_display {
   int ref_count;
   struct hgl_display *disp;
   struct sw_winsys *winsys;
};

struct haiku_egl_config {
   struct _egl_config base;
};

struct haiku_egl_context {
   struct _egl_context base;
   struct hgl_context *ctx;
};

struct haiku_egl_surface {
   struct _egl_surface base;
   struct hgl_buffer *fb;
   struct pipe_fence_handle *throttle_fence;
   HaikuNativeSurface *native_surface;
   bool is_window;
};

// #pragma mark EGLSurface

// Called via eglCreateWindowSurface(), drv->CreateWindowSurface().
static struct _egl_surface *
haiku_create_window_surface(struct _egl_display *disp, struct _egl_config *conf,
                            void *native_window, const EGLint *attrib_list)
{
   if (native_window == NULL) {
      _eglError(EGL_BAD_NATIVE_WINDOW, "Haiku: native window is NULL");
      return NULL;
   }

   printf("haiku_create_window_surface\n");
   struct haiku_egl_display *hgl_dpy = haiku_egl_display(disp);
   if (hgl_dpy == NULL || hgl_dpy->disp == NULL)
      return NULL;

   struct haiku_egl_surface *wgl_surf =
      (struct haiku_egl_surface *)calloc(1, sizeof(*wgl_surf));
   if (!wgl_surf)
      return NULL;

   if (!_eglInitSurface(&wgl_surf->base, disp, EGL_WINDOW_BIT, conf,
                        attrib_list, NULL)) {
      free(wgl_surf);
      return NULL;
   }

   struct st_visual visual;
   hgl_get_st_visual(&visual, haiku_visual_options(conf, true));

   wgl_surf->native_surface = new (std::nothrow)
      HaikuNativeSurface((BitmapHook *)native_window);
   if (wgl_surf->native_surface == NULL
       || !wgl_surf->native_surface->IsValid()) {
      delete wgl_surf->native_surface;
      free(wgl_surf);
      _eglError(EGL_BAD_ALLOC, "Haiku: creating native surface wrapper");
      return NULL;
   }

   wgl_surf->is_window = true;
   wgl_surf->fb = hgl_create_st_framebuffer(hgl_dpy->disp, &visual,
                                             wgl_surf->native_surface);
   if (!wgl_surf->fb) {
      delete wgl_surf->native_surface;
      free(wgl_surf);
      return NULL;
   }

   return &wgl_surf->base;
}

static struct _egl_surface *
haiku_create_pixmap_surface(struct _egl_display *disp, struct _egl_config *conf,
                            void *native_pixmap, const EGLint *attrib_list)
{
   printf("haiku_create_pixmap_surface\n");
   return NULL;
}

static struct _egl_surface *
haiku_create_pbuffer_surface(struct _egl_display *disp, struct _egl_config *conf,
                             const EGLint *attrib_list)
{
   printf("haiku_create_pbuffer_surface\n");
   struct haiku_egl_display *hgl_dpy = haiku_egl_display(disp);
   if (hgl_dpy == NULL || hgl_dpy->disp == NULL)
      return NULL;

   struct haiku_egl_surface *wgl_surf =
      (struct haiku_egl_surface *)calloc(1, sizeof(*wgl_surf));
   if (!wgl_surf)
      return NULL;

   if (!_eglInitSurface(&wgl_surf->base, disp, EGL_PBUFFER_BIT, conf,
                        attrib_list, NULL)) {
      free(wgl_surf);
      return NULL;
   }

   struct st_visual visual;
   hgl_get_st_visual(&visual, haiku_visual_options(conf, false));

   wgl_surf->is_window = false;
   wgl_surf->fb = hgl_create_st_framebuffer(hgl_dpy->disp, &visual, NULL);
   if (!wgl_surf->fb) {
      free(wgl_surf);
      return NULL;
   }

   wgl_surf->fb->newWidth = wgl_surf->base.Width;
   wgl_surf->fb->newHeight = wgl_surf->base.Height;
   p_atomic_inc(&wgl_surf->fb->base.stamp);

   return &wgl_surf->base;
}

static EGLBoolean
haiku_destroy_surface(struct _egl_display *disp, struct _egl_surface *surf)
{
   if (_eglPutSurface(surf)) {
      struct haiku_egl_surface *hgl_surf = haiku_egl_surface(surf);
      struct haiku_egl_display *hgl_dpy = haiku_egl_display(disp);

      if (hgl_dpy != NULL && hgl_dpy->disp != NULL) {
         struct pipe_screen *screen = hgl_dpy->disp->fscreen->screen;
         screen->fence_reference(screen, &hgl_surf->throttle_fence, NULL);
      }

      /*
       * The winsys owns each BBitmap. Detach the native view before the
       * framebuffer releases its display-target references.
       */
      if (hgl_surf->is_window && hgl_surf->native_surface != NULL)
         hgl_surf->native_surface->SetBitmap(NULL);

      if (hgl_surf->fb != NULL)
         hgl_destroy_st_framebuffer(hgl_surf->fb);
      delete hgl_surf->native_surface;
      free(surf);
   }
   return EGL_TRUE;
}

static void
update_size(struct hgl_buffer *buffer)
{
   uint32_t newWidth, newHeight;
   BitmapHook *nativeSurface;

   if (buffer == NULL || buffer->winsysContext == NULL
       || !buffer->mutex_initialized)
      return;

   nativeSurface = (BitmapHook *)buffer->winsysContext;
   nativeSurface->GetSize(newWidth, newHeight);

   mtx_lock(&buffer->mutex);
   if (buffer->newWidth != newWidth || buffer->newHeight != newHeight) {
      /* Detach before validation releases the old display-target bitmap. */
      nativeSurface->SetBitmap(NULL);
      buffer->newWidth = newWidth;
      buffer->newHeight = newHeight;
      p_atomic_inc(&buffer->base.stamp);
   }
   mtx_unlock(&buffer->mutex);
}

static EGLBoolean
haiku_swap_buffers(struct _egl_display *disp, struct _egl_surface *surf)
{
   struct haiku_egl_display *hgl_dpy = haiku_egl_display(disp);
   struct haiku_egl_surface *hgl_surf = haiku_egl_surface(surf);
   struct hgl_buffer *buffer = hgl_surf->fb;
   struct haiku_egl_context *hgl_ctx;
   struct pipe_resource *back_buffer = NULL;
   struct st_context *st;
   struct pipe_screen *screen;
   struct pipe_fence_handle *new_fence = NULL;

   if (buffer == NULL)
      return EGL_FALSE;

   /* eglSwapBuffers on a pbuffer has no native presentation operation. */
   if (buffer->winsysContext == NULL)
      return EGL_TRUE;

   hgl_ctx = haiku_egl_context(surf->CurrentContext);
   if (hgl_ctx == NULL)
      return EGL_FALSE;

   st = hgl_ctx->ctx->st;
   screen = hgl_dpy->disp->fscreen->screen;

   /*
    * Take a resource reference while holding the drawable lock. Do not hold
    * this lock while flushing Mesa: the flush path can call back into HGL.
    */
   mtx_lock(&buffer->mutex);
   pipe_resource_reference(&back_buffer,
      buffer->textures[ST_ATTACHMENT_BACK_LEFT]);
   mtx_unlock(&buffer->mutex);

   if (back_buffer == NULL)
      return EGL_TRUE;

   st->pipe->flush_resource(st->pipe, back_buffer);
   _mesa_glthread_finish(st->ctx);

   st_context_flush(st, ST_FLUSH_FRONT, &new_fence, NULL, NULL);

   if (hgl_surf->throttle_fence != NULL) {
      if (!screen->fence_finish(screen, NULL, hgl_surf->throttle_fence,
                                OS_TIMEOUT_INFINITE)) {
         screen->fence_reference(screen, &new_fence, NULL);
         pipe_resource_reference(&back_buffer, NULL);
         return EGL_FALSE;
      }
      screen->fence_reference(screen, &hgl_surf->throttle_fence, NULL);
   }
   hgl_surf->throttle_fence = new_fence;

   /* Present the referenced back buffer before changing the drawable pair. */
   screen->flush_frontbuffer(screen, st->pipe, back_buffer, 0, 0,
                             buffer->winsysContext, 1, NULL);

   mtx_lock(&buffer->mutex);
   if (buffer->textures[ST_ATTACHMENT_BACK_LEFT] == back_buffer) {
      std::swap(buffer->textures[ST_ATTACHMENT_FRONT_LEFT],
                buffer->textures[ST_ATTACHMENT_BACK_LEFT]);
      p_atomic_inc(&buffer->base.stamp);
   }
   mtx_unlock(&buffer->mutex);

   pipe_resource_reference(&back_buffer, NULL);

   update_size(buffer);
   st_context_invalidate_state(st, ST_INVALIDATE_FB_STATE);

   return EGL_TRUE;
}

static EGLBoolean
haiku_query_surface(struct _egl_display *disp, struct _egl_surface *surf,
                    EGLint attribute, EGLint *value)
{
   return _eglQuerySurface(disp, surf, attribute, value);
}


static EGLBoolean
haiku_swap_interval(struct _egl_display *disp, struct _egl_surface *surf,
                    EGLint interval)
{
   (void)disp;

   if (interval < surf->Config->MinSwapInterval
       || interval > surf->Config->MaxSwapInterval)
      return _eglError(EGL_BAD_PARAMETER, "Haiku: invalid swap interval");

   /*
    * BitmapHook has no portable compositor/vblank primitive. Keep the EGL
    * interval state but do not fake synchronization with a CPU sleep.
    */
   surf->SwapInterval = interval;
   return EGL_TRUE;
}


// #pragma mark EGLDisplay

static bool
haiku_format_supported(struct pipe_screen *screen, enum pipe_format format,
                       unsigned bind)
{
   return screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 0, 0,
                                      bind);
}


static bool
haiku_config_supported(struct pipe_screen *screen, EGLint alpha,
                       EGLint depth, EGLint stencil)
{
   enum pipe_format color = alpha ? PIPE_FORMAT_BGRA8888_UNORM
                                  : PIPE_FORMAT_BGRX8888_UNORM;

   if (!haiku_format_supported(screen, color, PIPE_BIND_RENDER_TARGET |
                                           PIPE_BIND_DISPLAY_TARGET))
      return false;

   enum pipe_format depthStencil = PIPE_FORMAT_NONE;
   if (depth && stencil)
      depthStencil = PIPE_FORMAT_Z24_UNORM_S8_UINT;
   else if (depth)
      depthStencil = PIPE_FORMAT_Z24X8_UNORM;
   else if (stencil)
      depthStencil = PIPE_FORMAT_X24S8_UINT;

   if (depthStencil != PIPE_FORMAT_NONE
       && !haiku_format_supported(screen, depthStencil,
                                  PIPE_BIND_DEPTH_STENCIL))
      return false;

   return true;
}


static EGLBoolean
haiku_add_configs_for_visuals(struct _egl_display *disp,
                              struct pipe_screen *screen)
{
   const struct {
      EGLint id;
      EGLint alpha;
      EGLint depth;
      EGLint stencil;
   } variants[] = {
      { 1, 8, 24, 8 },
      { 2, 8, 24, 0 },
      { 3, 0, 24, 8 },
      { 4, 0, 24, 0 },
      { 5, 8, 0, 0 },
   };

   for (unsigned i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
      struct haiku_egl_config *conf;

      if (!haiku_config_supported(screen, variants[i].alpha,
                                  variants[i].depth, variants[i].stencil))
         continue;

      conf = (struct haiku_egl_config *)calloc(1, sizeof(*conf));
      if (conf == NULL)
         return _eglError(EGL_BAD_ALLOC, "Haiku: config allocation failed");

      _eglInitConfig(&conf->base, disp, variants[i].id);
      conf->base.RedSize = 8;
      conf->base.GreenSize = 8;
      conf->base.BlueSize = 8;
      conf->base.AlphaSize = variants[i].alpha;
      conf->base.ColorBufferType = EGL_RGB_BUFFER;
      conf->base.BufferSize = 24 + variants[i].alpha;
      conf->base.ConfigCaveat = EGL_NONE;
      conf->base.BindToTextureRGB = EGL_FALSE;
      conf->base.BindToTextureRGBA = EGL_FALSE;
      conf->base.DepthSize = variants[i].depth;
      conf->base.StencilSize = variants[i].stencil;
      conf->base.TransparentType = EGL_NONE;
      conf->base.NativeRenderable = EGL_TRUE;
      conf->base.NativeVisualID = 0;
      conf->base.NativeVisualType = EGL_NONE;
      conf->base.RenderableType = EGL_OPENGL_BIT;
      conf->base.SampleBuffers = 0;
      conf->base.Samples = 0;
      conf->base.Level = 0;
      conf->base.MaxPbufferWidth = _EGL_MAX_PBUFFER_WIDTH;
      conf->base.MaxPbufferHeight = _EGL_MAX_PBUFFER_HEIGHT;
      conf->base.MaxPbufferPixels = _EGL_MAX_PBUFFER_WIDTH *
                                    _EGL_MAX_PBUFFER_HEIGHT;
      conf->base.MinSwapInterval = 0;
      conf->base.MaxSwapInterval = 1;
      conf->base.SurfaceType = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;

      if (!_eglValidateConfig(&conf->base, EGL_FALSE)) {
         free(conf);
         continue;
      }

      _eglLinkConfig(&conf->base);
   }

   return _eglGetArraySize(disp->Configs) != 0;
}

static void
haiku_display_destroy(struct _egl_display *disp)
{
   struct haiku_egl_display *hgl_dpy;

   if (disp == NULL)
      return;

   hgl_dpy = haiku_egl_display(disp);
   if (hgl_dpy == NULL)
      return;

   assert(hgl_dpy->ref_count > 0);
   if (!p_atomic_dec_zero(&hgl_dpy->ref_count))
      return;

   if (hgl_dpy->disp != NULL) {
      struct pipe_screen *screen = hgl_dpy->disp->fscreen->screen;
      hgl_destroy_display(hgl_dpy->disp);
      hgl_dpy->disp = NULL;
      if (screen != NULL)
         screen->destroy(screen);
   }

   if (hgl_dpy->winsys != NULL) {
      hgl_dpy->winsys->destroy(hgl_dpy->winsys);
      hgl_dpy->winsys = NULL;
   }

   disp->DriverDisplay = NULL;
   free(hgl_dpy);
}

static EGLBoolean
haiku_initialize_impl(struct _egl_display *disp, void *platformDisplay)
{
   struct haiku_egl_display *hgl_dpy;
   struct sw_winsys *winsys;
   struct pipe_screen *screen;

   hgl_dpy = (struct haiku_egl_display *)calloc(1, sizeof(*hgl_dpy));
   if (hgl_dpy == NULL)
      return _eglError(EGL_BAD_ALLOC, "eglInitialize");

   winsys = hgl_create_sw_winsys();
   if (winsys == NULL) {
      free(hgl_dpy);
      return _eglError(EGL_BAD_ALLOC, "Haiku: creating software winsys");
   }

   screen = sw_screen_create(winsys);
   if (screen == NULL) {
      winsys->destroy(winsys);
      free(hgl_dpy);
      return _eglError(EGL_NOT_INITIALIZED, "Haiku: creating llvmpipe screen");
   }

   hgl_dpy->winsys = winsys;
   hgl_dpy->disp = hgl_create_display(screen);
   if (hgl_dpy->disp == NULL) {
      screen->destroy(screen);
      winsys->destroy(winsys);
      hgl_dpy->winsys = NULL;
      free(hgl_dpy);
      return _eglError(EGL_BAD_ALLOC, "Haiku: creating HGL display");
   }

   if (!haiku_add_configs_for_visuals(disp, screen)) {
      hgl_destroy_display(hgl_dpy->disp);
      hgl_dpy->disp = NULL;
      screen->destroy(screen);
      winsys->destroy(winsys);
      hgl_dpy->winsys = NULL;
      free(hgl_dpy);
      return _eglError(EGL_NOT_INITIALIZED,
                       "Haiku: required framebuffer formats unavailable");
   }

   disp->ClientAPIs = 0;
   if (_eglIsApiValid(EGL_OPENGL_API))
      disp->ClientAPIs |= EGL_OPENGL_BIT;

   hgl_dpy->ref_count = 1;
   disp->DriverDisplay = (void *)hgl_dpy;
   return EGL_TRUE;
}

static EGLBoolean
haiku_initialize(struct _egl_display *disp)
{
   EGLBoolean ret = EGL_FALSE;
   struct haiku_egl_display *hgl_dpy = haiku_egl_display(disp);

   if (hgl_dpy) {
      hgl_dpy->ref_count++;
      return EGL_TRUE;
   }

   switch (disp->Platform) {
   case _EGL_PLATFORM_SURFACELESS:
   case _EGL_PLATFORM_HAIKU:
      ret = haiku_initialize_impl(disp, NULL);
      break;
   case _EGL_PLATFORM_DEVICE:
      ret = haiku_initialize_impl(disp, disp->PlatformDisplay);
      break;
   default:
      UNREACHABLE("Callers ensure we cannot get here.");
      return EGL_FALSE;
   }

   if (!ret)
      return EGL_FALSE;

   hgl_dpy = haiku_egl_display(disp);

   return EGL_TRUE;
}

static EGLBoolean
haiku_terminate(struct _egl_display *disp)
{
   haiku_display_destroy(disp);
   return EGL_TRUE;
}

// #pragma mark EGLContext

static struct _egl_context *
haiku_create_context(struct _egl_display *disp, struct _egl_config *conf,
                     struct _egl_context *share_list, const EGLint *attrib_list)
{
   CALLED();

   struct haiku_egl_display *hgl_dpy = haiku_egl_display(disp);
   if (hgl_dpy == NULL || hgl_dpy->disp == NULL)
      return NULL;

   struct st_visual visual;
   hgl_get_st_visual(&visual, haiku_visual_options(conf, true));

   struct haiku_egl_context *context =
      (struct haiku_egl_context *)calloc(1, sizeof(*context));
   if (!context) {
      _eglError(EGL_BAD_ALLOC, "haiku_create_context");
      return NULL;
   }

   if (!_eglInitContext(&context->base, disp, conf, share_list, attrib_list))
      goto cleanup;

   struct hgl_context_attribs hgl_attribs = {};
   if (context->base.ClientAPI != EGL_OPENGL_API) {
      _eglError(EGL_BAD_API, "Haiku: only desktop OpenGL is supported");
      goto cleanup;
   }

   hgl_attribs.profile = API_OPENGL_COMPAT;
   hgl_attribs.major = context->base.ClientMajorVersion;
   hgl_attribs.minor = context->base.ClientMinorVersion;

   context->ctx = hgl_create_context_with_attribs(
      hgl_dpy->disp, &visual,
      share_list == NULL ? NULL : haiku_egl_context(share_list)->ctx->st,
      &hgl_attribs);
   if (context->ctx == NULL)
      goto cleanup;

   return &context->base;

cleanup:
   free(context);
   return NULL;
}

static EGLBoolean
haiku_destroy_context(struct _egl_display *disp, struct _egl_context *ctx)
{
   if (_eglPutContext(ctx)) {
      struct haiku_egl_context *hgl_ctx = haiku_egl_context(ctx);
      hgl_destroy_context(hgl_ctx->ctx);
      free(ctx);
      ctx = NULL;
   }
   return EGL_TRUE;
}

static EGLBoolean
haiku_make_current(struct _egl_display *disp, struct _egl_surface *dsurf,
                   struct _egl_surface *rsurf, struct _egl_context *ctx)
{
   struct haiku_egl_context *hgl_ctx =
      ctx != NULL ? haiku_egl_context(ctx) : NULL;
   struct haiku_egl_surface *hgl_dsurf =
      dsurf != NULL ? haiku_egl_surface(dsurf) : NULL;
   struct haiku_egl_surface *hgl_rsurf =
      rsurf != NULL ? haiku_egl_surface(rsurf) : NULL;
   struct _egl_context *old_ctx;
   struct _egl_surface *old_dsurf, *old_rsurf;

   CALLED();

   if ((dsurf != NULL && dsurf->Display != disp)
       || (rsurf != NULL && rsurf->Display != disp)) {
      _eglError(EGL_BAD_MATCH, "Haiku: surface belongs to another display");
      return EGL_FALSE;
   }
   if (ctx != NULL && (hgl_ctx == NULL || hgl_ctx->ctx == NULL)) {
      _eglError(EGL_BAD_CONTEXT, "Haiku: invalid context");
      return EGL_FALSE;
   }
   if ((dsurf != NULL && (hgl_dsurf == NULL || hgl_dsurf->fb == NULL))
       || (rsurf != NULL && (hgl_rsurf == NULL || hgl_rsurf->fb == NULL))) {
      _eglError(EGL_BAD_SURFACE, "Haiku: invalid surface");
      return EGL_FALSE;
   }

   if (!_eglBindContext(ctx, dsurf, rsurf, &old_ctx, &old_dsurf, &old_rsurf))
      return EGL_FALSE;

   if (old_ctx == ctx && old_dsurf == dsurf && old_rsurf == rsurf) {
      _eglPutSurface(old_dsurf);
      _eglPutSurface(old_rsurf);
      _eglPutContext(old_ctx);
      return EGL_TRUE;
   }

   if (ctx == NULL) {
      st_api_make_current(NULL, NULL, NULL);
   } else {
      if (hgl_dsurf != NULL && dsurf != old_dsurf)
         update_size(hgl_dsurf->fb);

      st_api_make_current(hgl_ctx->ctx->st,
                          hgl_dsurf == NULL ? NULL : &hgl_dsurf->fb->base,
                          hgl_rsurf == NULL ? NULL : &hgl_rsurf->fb->base);
   }

   if (old_dsurf != NULL)
      haiku_destroy_surface(disp, old_dsurf);
   if (old_rsurf != NULL)
      haiku_destroy_surface(disp, old_rsurf);
   if (old_ctx != NULL)
      haiku_destroy_context(disp, old_ctx);

   return EGL_TRUE;
}

extern "C" const struct _egl_driver Driver = {
   .Initialize = haiku_initialize,
   .Terminate = haiku_terminate,
   .CreateContext = haiku_create_context,
   .DestroyContext = haiku_destroy_context,
   .MakeCurrent = haiku_make_current,
   .CreateWindowSurface = haiku_create_window_surface,
   .CreatePixmapSurface = haiku_create_pixmap_surface,
   .CreatePbufferSurface = haiku_create_pbuffer_surface,
   .DestroySurface = haiku_destroy_surface,
   .QuerySurface = haiku_query_surface,
   .SwapInterval = haiku_swap_interval,
   .SwapBuffers = haiku_swap_buffers,
};
