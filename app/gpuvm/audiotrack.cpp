#include "audiotrack.h"

#include <opus_multistream.h>

#include <algorithm>

namespace GpuVM {

AudioTrack::~AudioTrack()
{
    if (m_Decoder != nullptr) {
        opus_multistream_decoder_destroy(m_Decoder);
    }
}

bool AudioTrack::open(const Mp4AudioInfo& opus)
{
    m_In = opus;
    if (opus.channels < 1 || opus.channels > 8 || opus.samplesPerPacket <= 0) {
        return false;
    }
    int error = 0;
    m_Decoder = opus_multistream_decoder_create(opus.sampleRate, opus.channels, opus.streams, opus.coupledStreams,
                                                opus.mapping, &error);
    if (m_Decoder == nullptr) {
        return false;
    }
    int channels = opus.channels <= 2 ? opus.channels : 6;
    if (!m_Encoder.open(channels, opus.sampleRate)) {
        return false;
    }
    m_Out = Mp4AudioInfo();
    m_Out.aac = true;
    m_Out.aacConfig = m_Encoder.audioSpecificConfig();
    m_Out.bitrate = m_Encoder.bitrate();
    m_Out.channels = channels;
    m_Out.sampleRate = opus.sampleRate;
    m_Out.samplesPerPacket = 1024;
    m_Pcm.resize((size_t)opus.samplesPerPacket * 4 * (size_t)opus.channels);
    return true;
}

void AudioTrack::push(const uint8_t* packet, size_t size, int64_t timeUs, std::vector<AacFrame>& out)
{
    if (m_Decoder == nullptr) {
        return;
    }
    int capacity = (int)(m_Pcm.size() / (size_t)m_In.channels);
    int frames = opus_multistream_decode(m_Decoder, packet, (opus_int32)size, m_Pcm.data(), capacity, 0);
    if (frames <= 0) {
        return;
    }
    const int16_t* pcm = m_Pcm.data();
    if (m_In.channels > 6) {
        // 7.1 (FL FR FC LFE BL BR SL SR, Windows' order) folded into 5.1: the sides join the backs.
        m_Mixed.resize((size_t)frames * 6);
        for (int i = 0; i < frames; i++) {
            const int16_t* s = &m_Pcm[(size_t)i * m_In.channels];
            int16_t* d = &m_Mixed[(size_t)i * 6];
            for (int c = 0; c < 4; c++) d[c] = s[c];
            d[4] = (int16_t)std::clamp((int)s[4] + (int)s[6], -32768, 32767);
            d[5] = (int16_t)std::clamp((int)s[5] + (int)s[7], -32768, 32767);
        }
        pcm = m_Mixed.data();
    }
    size_t before = out.size();
    m_Encoder.encode(pcm, frames, timeUs, out);
    for (size_t i = before; i < out.size(); i++) {
        out[i].timeUs -= kEncoderDelayUs;
    }
}

void AudioTrack::finish(std::vector<AacFrame>& out)
{
    size_t before = out.size();
    m_Encoder.drain(out);
    for (size_t i = before; i < out.size(); i++) {
        out[i].timeUs -= kEncoderDelayUs;
    }
}

}
