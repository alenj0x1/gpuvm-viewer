// GpuVM viewer: screenshots, recordings and the last seconds of the machine's picture.
//
// - Screenshot: the next decoded frame (the machine's own picture at its full resolution, never the panel or the
//   viewer's overlays) is copied by the renderer and saved as PNG on a worker thread.
// - Recording: the stream's own H.264/HEVC/AV1 is written to MP4 as it arrives (mp4writer.h), no re-encoding; the
//   sound goes in as AAC (audiotrack.h). It starts at a key frame (one is asked for) and a new file starts if the
//   stream's format changes.
// - Replay: when turned on, the last seconds of packets stay in memory (from a key frame on, capped), and can be
//   saved as a clip.
//
// The viewer runs at Low integrity and may only write under its LocalLow folder: files are written to
// <dataDirectory>\Captures as .part, renamed when finished, and announced to the app ("capture"), which moves them
// to the person's Pictures or Videos folder.

#pragma once

#include <SDL.h>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

struct _DECODE_UNIT;
struct _OPUS_MULTISTREAM_CONFIGURATION;

namespace GpuVM {

// A decoded frame copied out of the GPU by the renderer, for a screenshot.
struct FramePlanes {
    int width = 0;
    int height = 0;
    bool tenBit = false;           // P010 (16 bits per sample, value in the high 10) instead of NV12
    std::vector<uint8_t> luma;     // width * bytes per sample per row, height rows
    std::vector<uint8_t> chroma;   // interleaved U/V at half resolution
    int colorspace = 1;            // AVColorSpace
    bool fullRange = false;
    bool pq = false;               // HDR10 (SMPTE ST 2084)
};

class Capture
{
public:
    static Capture& instance();

    // Stream setup: the decoder (each time it is created) and the audio stream.
    void setVideoFormat(int videoFormat, int width, int height, int frameRate);
    void setAudioConfig(const _OPUS_MULTISTREAM_CONFIGURATION* config);

    // Every frame and audio packet as it arrives (decoder and audio threads). Nearly free while nothing is on.
    void onVideoFrame(const _DECODE_UNIT* du);
    void onAudioPacket(const char* data, int length);

    // Commands (main thread): from the app's panel and from the viewer's keys.
    void screenshot();
    void setRecording(bool on);
    void toggleRecording() { setRecording(!m_Recording.load()); }
    void saveReplay();
    // The app's settings: replay on or off, its length, and the language of the short notices over the picture.
    void configure(bool replay, int seconds, const QString& lang);

    // Renderer (render thread): whether it should copy the next frame it draws (a screenshot, or a small picture
    // for the panel to show a recording by), and the copy.
    bool screenshotWanted() const { return m_ShotWanted.load(std::memory_order_relaxed) || m_ThumbWanted.load(std::memory_order_relaxed); }
    void screenshotTaken(std::unique_ptr<FramePlanes> planes);
    void screenshotFailed(const char* reason);

    // The session is ending (main thread, before the bridge closes): finishes the recording and frees the buffer.
    void sessionEnded();

    // Bytes the replay buffer holds now (lab 044).
    size_t replayBytes() const { return m_ReplayBytes.load(); }

    // Internal state, shared with the threads that write the files (capture.cpp).
    struct Impl;
    Impl* impl();
    std::atomic<bool> m_ShotWanted{false};
    std::atomic<bool> m_ThumbWanted{false};
    std::atomic<bool> m_Recording{false};
    std::atomic<bool> m_Replay{false};
    std::atomic<size_t> m_ReplayBytes{0};

private:
    Capture() = default;
};

}
