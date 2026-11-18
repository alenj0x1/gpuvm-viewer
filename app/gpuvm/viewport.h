// GpuVM viewer: the part of the window the picture uses. When GpuVM's side panel opens it asks for a share of
// the window on the right; the picture slides to the left and keeps its aspect ratio instead of being covered.
// Renderers and the mouse/touch mapping call apply() on the full window rect before scaling the picture into
// it, so the picture and the pointer always agree.

#pragma once

#include <SDL.h>

namespace GpuVM {

class Viewport
{
public:
    // Share of the window width kept free on the right (0 to 0.5), reached over durationMs with an ease-out.
    // Called on the SDL main thread; wakes the renderer (see setRedrawListener) so the picture moves even when
    // no new frame arrives.
    static void setRightInset(float fraction, int durationMs);

    // Shrinks the full-window rect to the picture area. Cheap; safe from any thread.
    static void apply(SDL_Rect* window);

    // Same with the area the current animation ends at (the pointer lock is set once, for where it ends).
    static void applyTarget(SDL_Rect* window);

    // Changes whenever the area changes (also on every step of an animation), so renderers that cache the
    // picture's vertices know when to rebuild them.
    static unsigned generation();

    // Draws the last frame once more even if no new one arrives (a screenshot of a still screen). Main thread.
    static void requestRedraw();

    // True while the area is still moving: renderers keep presenting even if no new frame arrives.
    static bool animating();

    // The renderer drawing the picture registers here to be woken when the area starts moving, so it draws the
    // last frame again at the new place. Main thread only (where renderers are created and destroyed).
    static void setRedrawListener(void (*listener)(void*), void* context);
    static void clearRedrawListener(void* context);
};

}
