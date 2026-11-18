// GpuVM viewer: AAC for the recordings' sound, through Windows' own AAC encoder (Media Foundation).
//
// The machine sends Opus, which Windows' players can't play inside an MP4 (lab 044), so the recorder decodes it and
// encodes AAC-LC, which every player opens. Nothing is installed: the encoder ships with Windows.

#pragma once

#include <cstdint>
#include <vector>

struct IMFTransform;

namespace GpuVM {

struct AacFrame {
    std::vector<uint8_t> data;
    int64_t timeUs;
};

class AacEncoder
{
public:
    AacEncoder() = default;
    ~AacEncoder();
    AacEncoder(const AacEncoder&) = delete;
    AacEncoder& operator=(const AacEncoder&) = delete;

    // 1, 2 or 6 channels at 44.1 or 48 kHz. Call on the thread that encodes (COM is set up there).
    bool open(int channels, int sampleRate);

    // Interleaved 16-bit samples starting at timeUs. Finished frames are appended to out with their own times.
    bool encode(const int16_t* pcm, int frames, int64_t timeUs, std::vector<AacFrame>& out);

    // The frames still inside the encoder.
    void drain(std::vector<AacFrame>& out);

    // AudioSpecificConfig for the MP4 (esds).
    std::vector<uint8_t> audioSpecificConfig() const;
    int bitrate() const { return m_Bitrate; }
    int channels() const { return m_Channels; }

private:
    bool pull(std::vector<AacFrame>& out);
    void close();

    IMFTransform* m_Encoder = nullptr;
    bool m_Started = false;
    bool m_Com = false;
    int m_Channels = 0;
    int m_SampleRate = 0;
    int m_Bitrate = 0;
    unsigned m_OutputSize = 0;
};

}
