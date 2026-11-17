// GpuVM viewer: what a streaming session tells the GpuVM app. Upstream files only call these functions, so the
// fork's footprint in them stays a few lines each (easier to carry to every new Moonlight version).

#pragma once

#include <SDL.h>

class Session;

namespace GpuVM {

// Right after a session object exists: forwards its stages, errors and end to the app. When the session is done
// the viewer sends "ended" (reason "closed", or "error" after a failure) and exits with 0 or 1.
void attachSession(Session* session);

// The streaming window was created, moved, resized or changed mode (its actual SDL flags are reported).
void reportWindow(SDL_Window* window);

// The first picture was presented (the app swaps its waiting screen for the stream at this moment).
void reportFirstFrame();

// Once a second while streaming: what the panel shows.
void reportStats(double receivedFps, double renderedFps, double networkDropPercent, double decodeMs,
                 double renderMs, double hostLatencyMs, double networkLatencyMs);

// The session ended: "closed" or "error". Sent once.
void reportEnded(const char* reason);

}
