// GpuVM viewer: a light copy of the stream for the recordings GpuVM keeps of agents' work.
//
// The stream comes at the host's refresh rate (180 fps) and ~16-21 Mb/s, which is what the person needs to use the
// machine live but ~7 GB per hour of agent work on disk (lab 057). For those recordings the recorder decodes the
// stream on its own (the GPU's decoder through D3D11VA, the picture as the viewer gets it), keeps one frame every
// 1/fps s (30 fps), and encodes them again as H.264 with Windows' own encoder (Media Foundation, on the CPU: the GPU's
// encoder is shared with the machines' streams through GPU-P) at a constant quality, so a still desktop costs almost
// nothing and a menu opening stays sharp. Lab 061: 4.6-15 MB per minute instead of ~160 (10-35x smaller), SSIM 0.99
// against the same frames nearly lossless. H.264 so every Windows player opens the file.
//
// What the person sees live is untouched: the renderer's decoder and picture are not shared with this. Only 8-bit
// 4:2:0 streams (H.264, HEVC, AV1); others are recorded as they come (the caller checks `supported`).
//
// Plain C++ with FFmpeg's decoders and Media Foundation, no Qt: lab 061 runs it on its own.

#pragma once

#include "mp4writer.h"

#include <cstdint>
#include <string>
#include <vector>

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct IMFTransform;

namespace GpuVM {

struct EncodedFrame {
    std::vector<uint8_t> data;  // H.264 Annex B; key frames carry their SPS and PPS
    int64_t timeUs = 0;
    bool key = false;
};

class Transcoder
{
public:
    struct Settings {
        int fps = 30;
        int bitrate = 2'000'000;
        int keySeconds = 4;
        // > 0: constant quality (CODECAPI_AVEncCommonQuality, 1..100) instead of a mean bitrate; the bitrate then
        // follows the picture (a still desktop costs almost nothing, a menu opening gets what it needs).
        // Lab 061: 60 kept text sharp through menus opening (no frame under SSIM 0.95, worst 1 % 0.986) at 4.6 to
        // 15 MB per minute; a 2 Mb/s mean blurred them for about a second (562 frames under 0.95 in 2 min).
        int quality = 60;
    };

    Transcoder() = default;
    ~Transcoder();
    Transcoder(const Transcoder&) = delete;
    Transcoder& operator=(const Transcoder&) = delete;

    // Whether a stream of this format can be recorded light (Limelight's VIDEO_FORMAT_* bits): 8-bit 4:2:0 only.
    static bool supported(int videoFormat);

    // On the thread that will push (COM is set up there). False (see error) if the decoder or encoder can't open.
    bool open(VideoCodec codec, int width, int height, const Settings& settings);

    // One frame as the stream carries it, on the stream's clock. The re-encoded frames that are ready are appended to
    // out. Frames before the first key frame are skipped; a frame the decoder can't read is skipped (the next key
    // frame repairs the picture). False only if the encoder fails.
    bool push(const uint8_t* data, size_t size, int64_t timeUs, bool key, std::vector<EncodedFrame>& out);

    // The frames still inside the decoder and the encoder.
    void drain(std::vector<EncodedFrame>& out);

    const std::string& error() const { return m_Error; }
    bool hardwareDecode() const { return m_HwDevice != nullptr; }

    // For the viewer's log and lab 061.
    struct Counts { int in = 0, decoded = 0, badPackets = 0, kept = 0, encoded = 0, keys = 0; int64_t bytes = 0; };
    const Counts& counts() const { return m_Counts; }
    std::string summary() const;

private:
    bool openDecoder(VideoCodec codec);
    bool openEncoder();
    bool receiveFrames(std::vector<EncodedFrame>& out);
    bool encode(const AVFrame* frame, int64_t timeUs, std::vector<EncodedFrame>& out);
    bool pull(std::vector<EncodedFrame>& out);
    void close();

    Settings m_Settings;
    int m_Width = 0, m_Height = 0;
    std::string m_Error;
    Counts m_Counts;

    AVCodecContext* m_Decoder = nullptr;
    AVBufferRef* m_HwDevice = nullptr;
    AVPacket* m_Packet = nullptr;
    AVFrame* m_Frame = nullptr;
    AVFrame* m_SwFrame = nullptr;
    bool m_SawKey = false;
    int64_t m_NextUs = INT64_MIN;  // the next frame kept is the first at or after this time

    IMFTransform* m_Encoder = nullptr;
    bool m_Com = false;
    bool m_Mf = false;
    bool m_EncoderProvidesSamples = false;
    unsigned m_OutputSize = 0;
    std::vector<uint8_t> m_SequenceHeader;  // SPS + PPS as the encoder reports them (Annex B)
};

}
