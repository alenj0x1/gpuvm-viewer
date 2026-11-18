// GpuVM viewer: one recording or clip as it is written: the MP4 writer (mp4writer.h) and its sound
// (audiotrack.h: the stream's Opus as AAC). The machine sends no sound while it is quiet, so the stretches without any
// are filled with real silence: the sound track never has holes (Windows' players refuse a file whose sound track is
// empty). Plain C++, no Qt: lab 044 runs it on its own.

#pragma once

#include "audiotrack.h"
#include "mp4writer.h"

#include <string>
#include <vector>

namespace GpuVM {

class RecordFile
{
public:
    // opus: the stream's sound layout, or nullptr for a file without sound.
    bool open(const std::wstring& path, const Mp4VideoInfo& video, const Mp4AudioInfo* opus);
    bool addVideo(const uint8_t* data, size_t size, int64_t timeUs, bool key);
    bool addAudio(const uint8_t* packet, size_t size, int64_t timeUs);
    // Silence up to the last frame, the encoder's last frames, the writer closed.
    bool close();

    bool started() const { return m_Writer.started(); }
    int64_t durationMs() const { return m_Writer.durationMs(); }
    const std::string& error() const { return m_Writer.error(); }
    // One line about the sound for the viewer's log.
    std::string summary() const;

private:
    void fillSilence(int64_t untilUs, int64_t minimumUs);
    void write(const std::vector<AacFrame>& frames);

    Mp4Writer m_Writer;
    AudioTrack m_Track;
    bool m_HasAudio = false;
    std::vector<AacFrame> m_Frames;
    int64_t m_AudioClockUs = 0;   // sound (real or silence) is in the file up to here
    int64_t m_LastVideoUs = 0;
    int64_t m_PacketUs = 5000;
    int64_t m_SilenceUs = 0;
    int m_VideoIn = 0, m_AudioIn = 0, m_AacOut = 0;
};

}
