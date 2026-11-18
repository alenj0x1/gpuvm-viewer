// GpuVM viewer: the recordings' sound. The machine's Opus packets are decoded (libopus, the same as playback) and
// encoded as AAC (aacencoder.h), so the files play in Windows' own players too. Surround is kept as 5.1 (7.1 is
// folded into 5.1: Windows' AAC encoder takes 1, 2 or 6 channels).

#pragma once

#include "aacencoder.h"
#include "mp4writer.h"

#include <vector>

struct OpusMSDecoder;

namespace GpuVM {

class AudioTrack
{
public:
    AudioTrack() = default;
    ~AudioTrack();
    AudioTrack(const AudioTrack&) = delete;
    AudioTrack& operator=(const AudioTrack&) = delete;

    // The stream's Opus layout. False if this machine's layout can't be recorded.
    bool open(const Mp4AudioInfo& opus);

    // What the MP4 gets (AAC).
    const Mp4AudioInfo& info() const { return m_Out; }

    // One Opus packet at timeUs; finished AAC frames are appended to out.
    void push(const uint8_t* packet, size_t size, int64_t timeUs, std::vector<AacFrame>& out);
    void finish(std::vector<AacFrame>& out);

    // Silence from timeUs for this long: the machine sends no sound while it is quiet, and a sound track with holes
    // (or none at all) is something Windows' players refuse.
    void silence(int64_t timeUs, int64_t durationUs, std::vector<AacFrame>& out);

    // How far Windows' AAC encoder delays the sound (its priming samples): its frames are stamped this much earlier
    // so the sound stays on the picture. Measured in lab 044.
    static constexpr int64_t kEncoderDelayUs = 0;

private:
    OpusMSDecoder* m_Decoder = nullptr;
    AacEncoder m_Encoder;
    Mp4AudioInfo m_In;
    Mp4AudioInfo m_Out;
    std::vector<int16_t> m_Pcm;
    std::vector<int16_t> m_Mixed;
};

}
