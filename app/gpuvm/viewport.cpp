#include "viewport.h"

#include <cmath>

namespace GpuVM {

namespace {

SDL_SpinLock s_Lock = 0;
float s_From = 0.0f;
float s_To = 0.0f;
Uint32 s_Start = 0;
int s_Duration = 0;
unsigned s_Redraws = 0;

void (*s_RedrawListener)(void*) = nullptr;
void* s_RedrawContext = nullptr;

float clampFraction(float f)
{
    if (!(f >= 0.0f)) return 0.0f;  // also NaN
    return f > 0.5f ? 0.5f : f;
}

// Same curve as the panel in the GpuVM app (cubic-bezier(.2, .8, .2, 1)), close enough with a quintic ease-out.
float easeOut(float t)
{
    float u = 1.0f - t;
    return 1.0f - u * u * u * u * u;
}

float currentLocked(Uint32 now)
{
    if (s_Duration <= 0) return s_To;
    float t = (float)(now - s_Start) / (float)s_Duration;
    if (t >= 1.0f) return s_To;
    return s_From + (s_To - s_From) * easeOut(t);
}

void cut(SDL_Rect* window, float inset)
{
    if (inset > 0.0f) {
        window->w -= (int)std::lround(window->w * inset);
    }
}

}

void Viewport::setRightInset(float fraction, int durationMs)
{
    Uint32 now = SDL_GetTicks();
    SDL_AtomicLock(&s_Lock);
    s_From = currentLocked(now);
    s_To = clampFraction(fraction);
    s_Start = now;
    s_Duration = durationMs < 0 ? 0 : (durationMs > 1000 ? 1000 : durationMs);
    void (*listener)(void*) = s_RedrawListener;
    void* context = s_RedrawContext;
    SDL_AtomicUnlock(&s_Lock);

    // Registered and removed on this same thread, so it is still alive here.
    if (listener != nullptr) {
        listener(context);
    }
}

void Viewport::apply(SDL_Rect* window)
{
    SDL_AtomicLock(&s_Lock);
    float inset = currentLocked(SDL_GetTicks());
    SDL_AtomicUnlock(&s_Lock);
    cut(window, inset);
}

void Viewport::applyTarget(SDL_Rect* window)
{
    SDL_AtomicLock(&s_Lock);
    float inset = s_To;
    SDL_AtomicUnlock(&s_Lock);
    cut(window, inset);
}

unsigned Viewport::generation()
{
    SDL_AtomicLock(&s_Lock);
    float inset = currentLocked(SDL_GetTicks());
    unsigned redraws = s_Redraws;
    SDL_AtomicUnlock(&s_Lock);
    // Quantized to 1/4096 of the width: below a pixel on any real window (at most 2048, so the redraw count fits
    // above it).
    return (unsigned)std::lround(inset * 4096.0f) + (redraws << 12);
}

void Viewport::requestRedraw()
{
    SDL_AtomicLock(&s_Lock);
    s_Redraws++;
    void (*listener)(void*) = s_RedrawListener;
    void* context = s_RedrawContext;
    SDL_AtomicUnlock(&s_Lock);
    if (listener != nullptr) {
        listener(context);
    }
}

bool Viewport::animating()
{
    SDL_AtomicLock(&s_Lock);
    bool moving = s_Duration > 0 && (SDL_GetTicks() - s_Start) < (Uint32)s_Duration;
    SDL_AtomicUnlock(&s_Lock);
    return moving;
}

void Viewport::setRedrawListener(void (*listener)(void*), void* context)
{
    SDL_AtomicLock(&s_Lock);
    s_RedrawListener = listener;
    s_RedrawContext = context;
    SDL_AtomicUnlock(&s_Lock);
}

void Viewport::clearRedrawListener(void* context)
{
    SDL_AtomicLock(&s_Lock);
    if (s_RedrawContext == context) {
        s_RedrawListener = nullptr;
        s_RedrawContext = nullptr;
    }
    SDL_AtomicUnlock(&s_Lock);
}

}
