# Haiku BGLRenderer target

This target builds Mesa's legacy Haiku OpenGL Kit renderer add-on:

- Meson target: `libswpipe`
- Output: `libswpipe.so`
- Factory: `instantiate_gl_renderer(BGLView*, ulong, BGLDispatcher*)`
- Rendering backend: Gallium llvmpipe through the HGL frontend and Haiku software winsys

The add-on is separate from the EGL driver. Both use the same HGL frontend and
winsys implementation, but the BGLRenderer target implements the legacy
OpenGL Kit lifecycle.

## Supported path

- RGB and RGBA visuals
- RGB565 visual selection is intentionally rejected because HGL's RGB565
  mapping is not a palette-indexed BGL_INDEX implementation
- Single- and double-buffered rendering
- Depth and stencil attachments where llvmpipe reports format support
- Bitmap presentation through BGLView's app_server drawing path
- Deferred invalidation on a presentation worker, avoiding looper/display-lock
  inversion when the application swaps while holding LockGL()
- Resize invalidation and framebuffer resource replacement

## Explicit limitations

- No BGL_INDEX palette rendering
- No accumulation, overlay, or underlay planes
- No shared BGL contexts
- No direct-framebuffer mode
- No vblank guarantee; the legacy vsync argument is ignored rather than
  emulated with a CPU sleep
- CopyPixelsIn/CopyPixelsOut are not implemented

## Packaging

The target installs `libswpipe.so` into the normal library directory. The
HaikuPorts recipe should move that file into the OpenGL add-ons directory under
the user-facing name `Software Pipe`, matching the OpenGL Kit renderer roster.

## Validation still required

Build the target on Haiku with only Gallium llvmpipe enabled, then test:

1. BGLView creation and renderer loading.
2. Context make-current/unbind on repeated LockGL/UnlockGL cycles.
3. Single-buffer drawing and double-buffer swaps.
4. Window resize while drawing and repeated framebuffer replacement.
5. BGLView detach/destruction while the renderer has no current context.
6. Concurrent Draw, resize, and swap activity.
7. GL entrypoint dispatch and GetGLProcAddress.
8. Renderer loading from the installed OpenGL add-ons directory.

This source target has not yet been compiled or runtime-tested on Haiku.
