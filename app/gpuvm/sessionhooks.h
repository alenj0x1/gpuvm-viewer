// GpuVM viewer: what a streaming session tells the GpuVM app. Upstream files only call these functions, so the
// fork's footprint in them stays a few lines each (easier to carry to every new Moonlight version).

#pragma once

#include <SDL.h>

class Session;

namespace GpuVM {

// Right after a session object exists: forwards its stages, errors and end to the app.
void attachSession(Session* session);

// The streaming window was created, moved, resized or changed mode.
void reportWindow(SDL_Window* window, bool fullScreen);

// The first picture was decoded (the app swaps its waiting screen for the stream at this moment).
void reportFirstFrame();

// Once a second while streaming: what the panel shows.
void reportStats(double receivedFps, double renderedFps, double networkDropPercent, double decodeMs,
                 double renderMs, double hostLatencyMs, double networkLatencyMs);

// The session ended; error is empty when the person closed it.
void reportEnded(const char* reason);

}
