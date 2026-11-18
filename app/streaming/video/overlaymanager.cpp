#include "overlaymanager.h"
#include "path.h"

#include <cmath>

using namespace Overlay;

OverlayManager::OverlayManager() :
    m_Renderer(nullptr),
    m_FontData(Path::readDataFile("ModeSeven.ttf"))
{
    memset(m_Overlays, 0, sizeof(m_Overlays));

    m_Overlays[OverlayType::OverlayDebug].color = {0xD0, 0xD0, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayDebug].fontSize = 20;

    m_Overlays[OverlayType::OverlayStatusUpdate].color = {0xCC, 0x00, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayStatusUpdate].fontSize = 36;

    // GpuVM: sized to the screen (about 20 px at 1080p), in the app's text color.
    {
        SDL_DisplayMode mode = {};
        int height = SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.h > 0 ? mode.h : 1080;
        m_Overlays[OverlayType::OverlayGpuVMCapture].color = {0xEB, 0xE8, 0xE1, 0xFF};
        m_Overlays[OverlayType::OverlayGpuVMCapture].fontSize = qBound(16, height / 54, 48);
    }

    // While TTF will usually not be initialized here, it is valid for that not to
    // be the case, since Session destruction is deferred and could overlap with
    // the lifetime of a new Session object.
    //SDL_assert(TTF_WasInit() == 0);

    if (TTF_Init() != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TTF_Init() failed: %s",
                    TTF_GetError());
        return;
    }
}

OverlayManager::~OverlayManager()
{
    for (int i = 0; i < OverlayType::OverlayMax; i++) {
        if (m_Overlays[i].surface != nullptr) {
            SDL_FreeSurface(m_Overlays[i].surface);
        }
        if (m_Overlays[i].font != nullptr) {
            TTF_CloseFont(m_Overlays[i].font);
        }
    }

    TTF_Quit();

    // For similar reasons to the comment in the constructor, this will usually,
    // but not always, deinitialize TTF. In the cases where Session objects overlap
    // in lifetime, there may be an additional reference on TTF for the new Session
    // that means it will not be cleaned up here.
    //SDL_assert(TTF_WasInit() == 0);
}

bool OverlayManager::isOverlayEnabled(OverlayType type)
{
    return m_Overlays[type].enabled;
}

char* OverlayManager::getOverlayText(OverlayType type)
{
    return m_Overlays[type].text;
}

void OverlayManager::updateOverlayText(OverlayType type, const char* text)
{
    SDL_utf8strlcpy(m_Overlays[type].text, text, sizeof(m_Overlays[0].text));
    setOverlayTextUpdated(type);
}

int OverlayManager::getOverlayMaxTextLength()
{
    return sizeof(m_Overlays[0].text);
}

int OverlayManager::getOverlayFontSize(OverlayType type)
{
    return m_Overlays[type].fontSize;
}

SDL_Surface* OverlayManager::getUpdatedOverlaySurface(OverlayType type)
{
    // If a new surface is available, return it. If not, return nullptr.
    // Caller must free the surface on success.
    return (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, nullptr);
}

void OverlayManager::setOverlayTextUpdated(OverlayType type)
{
    // Only update the overlay state if it's enabled. If it's not enabled,
    // the renderer has already been notified by setOverlayState().
    if (m_Overlays[type].enabled) {
        notifyOverlayUpdated(type);
    }
}

void OverlayManager::setOverlayState(OverlayType type, bool enabled)
{
    bool stateChanged = m_Overlays[type].enabled != enabled;

    m_Overlays[type].enabled = enabled;

    if (stateChanged) {
        if (!enabled) {
            // Set the text to empty string on disable
            m_Overlays[type].text[0] = 0;
        }

        notifyOverlayUpdated(type);
    }
}

SDL_Color OverlayManager::getOverlayColor(OverlayType type)
{
    return m_Overlays[type].color;
}

void OverlayManager::setOverlayRenderer(IOverlayRenderer* renderer)
{
    m_Renderer = renderer;
}

void OverlayManager::notifyOverlayUpdated(OverlayType type)
{
    if (m_Renderer == nullptr) {
        return;
    }

    // GpuVM: the capture pill uses Windows' own UI font (read-only, in the Windows folder), like the app around it.
    if (type == OverlayGpuVMCapture && m_Overlays[type].font == nullptr) {
#ifdef Q_OS_WIN32
        const char* fonts[] = { "C:\\Windows\\Fonts\\seguisb.ttf", "C:\\Windows\\Fonts\\segoeui.ttf" };
        for (const char* path : fonts) {
            m_Overlays[type].font = TTF_OpenFont(path, m_Overlays[type].fontSize);
            if (m_Overlays[type].font != nullptr) {
                break;
            }
        }
#endif
    }

    // Construct the required font to render the overlay
    if (m_Overlays[type].font == nullptr) {
        if (m_FontData.isEmpty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL overlay font failed to load");
            return;
        }

        // m_FontData must stay around until the font is closed
        m_Overlays[type].font = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()),
                                               1,
                                               m_Overlays[type].fontSize);
        if (m_Overlays[type].font == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "TTF_OpenFont() failed: %s",
                        TTF_GetError());

            // Can't proceed without a font
            return;
        }
    }

    // Exchange the old surface with the new one
    SDL_Surface* oldSurface = (SDL_Surface*)SDL_AtomicSetPtr(
        (void**)&m_Overlays[type].surface,
        !m_Overlays[type].enabled ? nullptr :
        type == OverlayGpuVMCapture ? renderGpuVMPill(m_Overlays[type].font, m_Overlays[type].text) :
            // The _Wrapped variant is required for line breaks to work
            RenderTextOutlinedWrapped(m_Overlays[type].font,
                                      m_Overlays[type].text,
                                      m_Overlays[type].color,
                                      {0, 0, 0, 255},
                                      4,
                                      1024));

    // Notify the renderer
    m_Renderer->notifyOverlayUpdated(type);

    // Free the old surface
    if (oldSurface != nullptr) {
        SDL_FreeSurface(oldSurface);
    }
}

// GpuVM: a graphite pill with the text (and, for a recording, a red dot before it). Text starting with \x01 gets the
// dot. Straight alpha, ARGB8888 like the other overlays.
SDL_Surface* OverlayManager::renderGpuVMPill(TTF_Font* font, const char* text)
{
    if (text == nullptr || text[0] == '\0') {
        return nullptr;
    }
    bool dot = text[0] == '\x01';
    const char* label = dot ? text + 1 : text;
    SDL_Surface* rendered = TTF_RenderUTF8_Blended(font, label, m_Overlays[OverlayGpuVMCapture].color);
    if (rendered == nullptr) {
        return nullptr;
    }
    SDL_Surface* glyphs = SDL_ConvertSurfaceFormat(rendered, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(rendered);
    if (glyphs == nullptr) {
        return nullptr;
    }
    int h = glyphs->h + glyphs->h / 2;
    float radius = h / 2.0f;
    int padX = h / 2;
    int dotD = dot ? h * 9 / 25 : 0;
    int gap = dot ? h * 7 / 25 : 0;
    int w = padX * 2 + dotD + gap + glyphs->w;
    SDL_Surface* pill = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (pill == nullptr) {
        SDL_FreeSurface(glyphs);
        return nullptr;
    }
    float dotX = padX + dotD / 2.0f, dotR = dotD / 2.0f, cy = h / 2.0f;
    for (int y = 0; y < h; y++) {
        Uint32* row = (Uint32*)((Uint8*)pill->pixels + y * pill->pitch);
        for (int x = 0; x < w; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            // Distance to the capsule's center line, antialiased over one pixel.
            float sx = qBound(radius, px, (float)w - radius);
            float dist = std::sqrt((px - sx) * (px - sx) + (py - cy) * (py - cy));
            float cover = qBound(0.0f, radius - dist + 0.5f, 1.0f);
            float a = 0.86f * cover;
            float r = 0x1c, g = 0x1c, b = 0x1c;
            if (dot) {
                float dd = std::sqrt((px - dotX) * (px - dotX) + (py - cy) * (py - cy));
                float dc = qBound(0.0f, dotR - dd + 0.5f, 1.0f);
                r += (0xe5 - r) * dc; g += (0x48 - g) * dc; b += (0x3f - b) * dc;
                a = a + (cover - a) * dc;
            }
            row[x] = ((Uint32)(a * 255.0f + 0.5f) << 24) | ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
        }
    }
    SDL_SetSurfaceBlendMode(glyphs, SDL_BLENDMODE_BLEND);
    SDL_Rect at = { padX + dotD + gap, (h - glyphs->h) / 2, glyphs->w, glyphs->h };
    SDL_BlitSurface(glyphs, nullptr, pill, &at);
    SDL_FreeSurface(glyphs);
    return pill;
}

SDL_Surface* OverlayManager::RenderTextOutlinedWrapped(TTF_Font* font, const char* text, SDL_Color textColor, SDL_Color outlineColor, int outlineWidth, int wrapWidth) {
    if (text == nullptr || text[0] == '\0') {
        return nullptr;
    }

    int oldOutline = TTF_GetFontOutline(font);
    TTF_SetFontOutline(font, outlineWidth);

    // Verify that the string won't require wrapping (which could cause the outline and the text
    // to diverge due to different wrapping positions).
    //
    // FIXME: We do this rather than just disabling wrapping entirely (wrapWidth = 0) because we
    // need further testing to ensure that all renderers can handle non-NPOT overlay textures.
    for (const QString& line : QString(text).split('\n')) {
        int extent, count;
        if (TTF_MeasureUTF8(font, line.toUtf8(), wrapWidth, &extent, &count) == 0 && count < line.size()) {
            // If it requires wrapping, render it without the outline
            TTF_SetFontOutline(font, oldOutline);
            return TTF_RenderUTF8_Blended_Wrapped(font, text, textColor, wrapWidth);
        }
    }

    // Draw text twice, but outline is a bit bigger
    auto outlineSurface = TTF_RenderUTF8_Blended_Wrapped(font, text, outlineColor, wrapWidth);
    TTF_SetFontOutline(font, 0);
    auto textSurface = TTF_RenderUTF8_Blended_Wrapped(font, text, textColor, wrapWidth);
    TTF_SetFontOutline(font, oldOutline);

    if (outlineSurface == nullptr || textSurface == nullptr) {
        SDL_FreeSurface(outlineSurface);
        SDL_FreeSurface(textSurface);
        return nullptr;
    }

    // Merge the texts
    SDL_Rect dst = { outlineWidth, outlineWidth, textSurface->w, textSurface->h };
    SDL_BlitSurface(textSurface, nullptr, outlineSurface, &dst);

    SDL_FreeSurface(textSurface);
    return outlineSurface;
}


