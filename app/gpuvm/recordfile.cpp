#include "recordfile.h"

#include <algorithm>
#include <cstdio>

namespace GpuVM {

namespace {
// Silence goes in only after this long without sound, and stops this much before the newest frame (sound that arrives
// a little late still finds its place).
constexpr int64_t kSilenceAfterUs = 100000;
constexpr int64_t kSilenceLeadUs = 60000;
constexpr int64_t kResumeGapUs = 40000;
}

bool RecordFile::open(const std::wstring& path, const Mp4VideoInfo& video, const Mp4AudioInfo* opus)
{
    m_HasAudio = opus != nullptr && m_Track.open(*opus);
    if (m_HasAudio && opus->sampleRate > 0) {
        m_PacketUs = (int64_t)opus->samplesPerPacket * 1000000 / opus->sampleRate;
    }
    return m_Writer.open(path, video, m_HasAudio ? &m_Track.info() : nullptr);
}

void RecordFile::write(const std::vector<AacFrame>& frames)
{
    m_AacOut += (int)frames.size();
    for (const AacFrame& f : frames) {
        m_Writer.addAudio(f.data.data(), f.data.size(), f.timeUs);
    }
}

void RecordFile::fillSilence(int64_t untilUs, int64_t minimumUs)
{
    if (untilUs - m_AudioClockUs <= minimumUs) {
        return;
    }
    m_Frames.clear();
    m_Track.silence(m_AudioClockUs, untilUs - m_AudioClockUs, m_Frames);
    m_SilenceUs += untilUs - m_AudioClockUs;
    write(m_Frames);
    m_AudioClockUs = untilUs;
}

bool RecordFile::addVideo(const uint8_t* data, size_t size, int64_t timeUs, bool key)
{
    m_VideoIn++;
    bool ok = m_Writer.addVideo(data, size, timeUs, key);
    if (m_Writer.started()) {
        m_LastVideoUs = timeUs;
        if (m_HasAudio) {
            if (m_AudioClockUs == 0) {
                m_AudioClockUs = timeUs;
            }
            fillSilence(timeUs - kSilenceLeadUs, kSilenceAfterUs);
        }
    }
    return ok;
}

bool RecordFile::addAudio(const uint8_t* packet, size_t size, int64_t timeUs)
{
    m_AudioIn++;
    if (!m_HasAudio) {
        return true;
    }
    // Sound coming back after a quiet stretch: silence exactly up to it, so it lands on its time. (While sound flows,
    // packets come every few ms; arrival jitter stays well under this.)
    if (m_AudioClockUs != 0 && timeUs - m_AudioClockUs > kResumeGapUs) {
        fillSilence(timeUs, 0);
    }
    m_AudioClockUs = (std::max)(m_AudioClockUs, timeUs + m_PacketUs);
    m_Frames.clear();
    m_Track.push(packet, size, timeUs, m_Frames);
    write(m_Frames);
    return m_Writer.error().empty();
}

bool RecordFile::close()
{
    if (m_HasAudio) {
        if (m_Writer.started() && m_AudioClockUs != 0) {
            fillSilence(m_LastVideoUs, kSilenceAfterUs);
        }
        m_Frames.clear();
        m_Track.finish(m_Frames);
        write(m_Frames);
    }
    return m_Writer.close();
}

std::string RecordFile::summary() const
{
    const Mp4Writer::AudioCounts& a = m_Writer.audioCounts();
    char line[320];
    snprintf(line, sizeof(line),
             "video in %d, sound %s: packets in %d, aac out %d, kept %d, early %d, ahead %d, gaps %d, silence %lld ms; %lld ms, error '%s'",
             m_VideoIn, m_HasAudio ? "on" : "off", m_AudioIn, m_AacOut, a.kept, a.early, a.ahead, a.gaps,
             (long long)(m_SilenceUs / 1000), (long long)m_Writer.durationMs(), m_Writer.error().c_str());
    return line;
}

}
