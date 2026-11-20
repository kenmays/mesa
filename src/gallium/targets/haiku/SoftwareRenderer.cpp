/*
 * Haiku BGLRenderer adapter for Mesa's HGL frontend.
 *
 * This is the legacy OpenGL Kit entry point. EGL remains a separate frontend;
 * this adapter deliberately uses the same HGL winsys and llvmpipe screen.
 */

#include <stdint.h>
#include <stdio.h>
#include <new>
#include <algorithm>

#include <GLRenderer.h>
#include <GLView.h>
#include <Bitmap.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "util/u_atomic.h"
#include "util/u_thread.h"
#include "state_tracker/st_context.h"
#include "hgl_context.h"
#include "hgl_sw_winsys.h"

extern "C" {
#include "target-helpers/inline_sw_helper.h"
}

class HaikuSoftwareRenderer final : public BGLRenderer, public BitmapHook {
public:
   HaikuSoftwareRenderer(BGLView *view, ulong options,
                         BGLDispatcher *dispatcher)
      : BGLRenderer(view, options, dispatcher),
        fScreen(nullptr), fDisplay(nullptr),
        fFramebuffer(nullptr), fContext(nullptr), fBitmap(nullptr),
        fMutexInitialized(false), fReady(false), fCurrent(false),
        fPendingPresent(false),
        fWidth(0), fHeight(0)
   {
      if (view == nullptr || dispatcher == nullptr
          || (options & (HGL_INDEX | HGL_ACCUM | HGL_OVERLAY
              | HGL_UNDERLAY | HGL_SHARE_CONTEXT)) != 0)
         return;
      if (mtx_init(&fMutex, mtx_recursive) != thrd_success)
         return;
      if (mtx_init(&fBitmapMutex, mtx_plain) != thrd_success) {
         mtx_destroy(&fMutex);
         return;
      }
      fMutexInitialized = true;

      BRect initialBounds = view->Bounds();
      if (initialBounds.IsValid()) {
         fWidth = (uint32_t)initialBounds.IntegerWidth() + 1;
         fHeight = (uint32_t)initialBounds.IntegerHeight() + 1;
      }

      struct sw_winsys *winsys = hgl_create_sw_winsys();
      if (winsys == nullptr)
         return;

      fScreen = sw_screen_create(winsys);
      if (fScreen == nullptr) {
         winsys->destroy(winsys);
         return;
      }
      /* sw_screen_create transfers winsys ownership to the screen. */

      fDisplay = hgl_create_display(fScreen);
      if (fDisplay == nullptr)
         return;

      hgl_get_st_visual(&fVisual, options);
      if (fVisual.color_format == PIPE_FORMAT_NONE)
         return;

      fFramebuffer = hgl_create_st_framebuffer(fDisplay, &fVisual, this);
      if (fFramebuffer == nullptr)
         return;

      fContext = hgl_create_context(fDisplay, &fVisual, nullptr);
      if (fContext == nullptr)
         return;

      fFramebuffer->newWidth = fWidth;
      fFramebuffer->newHeight = fHeight;
      fReady = true;
   }

   ~HaikuSoftwareRenderer() override
   {
      if (fMutexInitialized) {
         mtx_lock(&fMutex);
         if (fCurrent) {
            MakeNotCurrent();
            /* Release the LockGL() acquisition as well as this one. */
            mtx_unlock(&fMutex);
         }
         mtx_unlock(&fMutex);
      }

      if (fContext != nullptr) {
         hgl_destroy_context(fContext);
         fContext = nullptr;
      }
      if (fFramebuffer != nullptr) {
         hgl_destroy_st_framebuffer(fFramebuffer);
         fFramebuffer = nullptr;
      }
      if (fDisplay != nullptr) {
         hgl_destroy_display(fDisplay);
         fDisplay = nullptr;
      }
      if (fScreen != nullptr) {
         fScreen->destroy(fScreen);
         fScreen = nullptr;
      }
      if (fMutexInitialized) {
         mtx_destroy(&fBitmapMutex);
         mtx_destroy(&fMutex);
         fMutexInitialized = false;
      }
   }

   bool IsReady() const { return fReady; }

   void LockGL() override
   {
      BGLRenderer::LockGL();
      if (!fReady || !fMutexInitialized)
         return;

      mtx_lock(&fMutex);
      mtx_lock(&fFramebuffer->mutex);
      fFramebuffer->newWidth = fWidth;
      fFramebuffer->newHeight = fHeight;
      mtx_unlock(&fFramebuffer->mutex);

      if (!st_api_make_current(fContext->st, &fFramebuffer->base,
                               &fFramebuffer->base)) {
         st_api_make_current(nullptr, nullptr, nullptr);
         mtx_unlock(&fMutex);
         return;
      }

      /* st_api_make_current binds Mesa's per-thread GL dispatch table. */

      fCurrent = true;
   }

   void UnlockGL() override
   {
      bool present = false;
      if (fReady && fCurrent) {
         if ((Options() & HGL_DOUBLE) == 0) {
            st_context_flush(fContext->st, ST_FLUSH_FRONT, nullptr, nullptr,
                             nullptr);
            present = true;
         } else if (fPendingPresent) {
            present = true;
         }
         fPendingPresent = false;
         MakeNotCurrent();
         mtx_unlock(&fMutex);
      }
      /* Never acquire the BWindow looper while holding the renderer lock. */
      if (present)
         PresentBitmap();
      BGLRenderer::UnlockGL();
   }

   void SwapBuffers(bool vsync = false) override
   {
      (void)vsync; /* BitmapHook exposes no vblank primitive. */
      if (!fReady)
         return;

      /*
       * The recursive mutex permits SwapBuffers both inside and outside
       * LockGL(), while serializing it against resize, context use and teardown.
       */
      mtx_lock(&fMutex);
      st_context_flush(fContext->st, 0, nullptr, nullptr, nullptr);

      struct pipe_resource *back = nullptr;
      mtx_lock(&fFramebuffer->mutex);
      pipe_resource_reference(&back,
         fFramebuffer->textures[ST_ATTACHMENT_BACK_LEFT]);
      mtx_unlock(&fFramebuffer->mutex);

      const bool didPresent = back != nullptr;
      if (back != nullptr) {
         fContext->st->pipe->flush_resource(fContext->st->pipe, back);
         fScreen->flush_frontbuffer(fScreen, fContext->st->pipe, back,
                                    0, 0, this, 1, nullptr);

         mtx_lock(&fFramebuffer->mutex);
         if (fFramebuffer->textures[ST_ATTACHMENT_BACK_LEFT] == back) {
            std::swap(fFramebuffer->textures[ST_ATTACHMENT_FRONT_LEFT],
                      fFramebuffer->textures[ST_ATTACHMENT_BACK_LEFT]);
            p_atomic_inc(&fFramebuffer->base.stamp);
         }
         mtx_unlock(&fFramebuffer->mutex);

         pipe_resource_reference(&back, nullptr);
         st_context_invalidate_state(fContext->st, ST_INVALIDATE_FB_STATE);
      }

      const bool presentNow = didPresent && !fCurrent;
      if (didPresent && fCurrent)
         fPendingPresent = true;
      mtx_unlock(&fMutex);
      /* Never acquire the BWindow looper while holding the renderer lock. */
      if (presentNow)
         PresentBitmap();
   }

   void Draw(BRect updateRect) override
   {
      (void)updateRect;
      BBitmap *bitmap;
      mtx_lock(&fBitmapMutex);
      bitmap = fBitmap;
      if (bitmap != nullptr)
         GLView()->DrawBitmap(bitmap, B_ORIGIN);
      mtx_unlock(&fBitmapMutex);
   }

   void FrameResized(float width, float height) override
   {
      if (!fReady)
         return;

      const unsigned newWidth = width >= 0 ? (unsigned)width + 1 : 0;
      const unsigned newHeight = height >= 0 ? (unsigned)height + 1 : 0;

      mtx_lock(&fMutex);
      fWidth = newWidth;
      fHeight = newHeight;
      mtx_lock(&fFramebuffer->mutex);
      fFramebuffer->newWidth = newWidth;
      fFramebuffer->newHeight = newHeight;
      p_atomic_inc(&fFramebuffer->base.stamp);
      mtx_unlock(&fFramebuffer->mutex);
      mtx_unlock(&fMutex);
   }

   void DirectConnected(direct_buffer_info *info) override
   {
      (void)info;
      /* Direct mode is intentionally not enabled by this adapter. */
   }

   void EnableDirectMode(bool enabled) override
   {
      (void)enabled;
      /* BGLView bitmap presentation remains the only supported path. */
   }

   status_t CopyPixelsOut(BPoint source, BBitmap *dest) override
   {
      (void)source;
      (void)dest;
      return B_ERROR;
   }

   status_t CopyPixelsIn(BBitmap *source, BPoint dest) override
   {
      (void)source;
      (void)dest;
      return B_ERROR;
   }

   void GetSize(uint32_t &width, uint32_t &height) override
   {
      mtx_lock(&fMutex);
      width = fWidth;
      height = fHeight;
      mtx_unlock(&fMutex);
   }

   BBitmap *SetBitmap(BBitmap *bitmap) override
   {
      BBitmap *previous;
      mtx_lock(&fBitmapMutex);
      previous = fBitmap;
      fBitmap = bitmap;
      mtx_unlock(&fBitmapMutex);
      return previous;
   }

private:
   void PresentBitmap()
   {
      if (GLView()->LockLooperWithTimeout(1000000) != B_OK)
         return;

      /*
       * BView drawing belongs in the view's Draw() callback. Invalidate
       * schedules that callback on the window looper without drawing from
       * the GL worker thread or bypassing BGLView's normal draw lifecycle.
       */
      GLView()->Invalidate();

      GLView()->UnlockLooper();
   }

   void MakeNotCurrent()
   {
      st_api_make_current(nullptr, nullptr, nullptr);
      fCurrent = false;
   }


   struct pipe_screen *fScreen;
   struct hgl_display *fDisplay;
   struct hgl_buffer *fFramebuffer;
   struct hgl_context *fContext;
   struct st_visual fVisual;
   BBitmap *fBitmap;
   mtx_t fMutex;
   mtx_t fBitmapMutex;
   bool fMutexInitialized;
   bool fReady;
   bool fCurrent;
   bool fPendingPresent;
   uint32_t fWidth;
   uint32_t fHeight;
};

extern "C" _EXPORT BGLRenderer *
instantiate_gl_renderer(BGLView *view, ulong options,
                        BGLDispatcher *dispatcher)
{
   HaikuSoftwareRenderer *renderer =
      new (std::nothrow) HaikuSoftwareRenderer(view, options, dispatcher);
   if (renderer == nullptr)
      return nullptr;

   if (!renderer->IsReady()) {
      delete renderer;
      return nullptr;
   }
   return renderer;
}
