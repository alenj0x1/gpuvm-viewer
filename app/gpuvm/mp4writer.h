// GpuVM viewer: writes the stream it receives into an MP4 file without decoding or encoding anything.
//
// The video is the machine's own H.264, HEVC or AV1 as it arrives; the audio is its Opus packets. The file is a
// fragmented MP4 (a fragment about every second and at every key frame): if the viewer stops abruptly, what was
// written up to the last fragment still plays. On close it gets its duration and an index of the key frames for
// seeking. The FFmpeg the viewer ships has no muxers, so this is our own (ISO/IEC 14496-12, -15, the AV1 and Opus
// ISOBMFF bindings).

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace GpuVM {

enum class VideoCodec { H264, HEVC, AV1 };

struct Mp4VideoInfo {
    VideoCodec codec = VideoCodec::H264;
    int width = 0;
    int height = 0;
    // ISO/IEC 23091-2 (colour primaries, transfer characteristics, matrix coefficients) for the colr box.
    int primaries = 1;
    int transfer = 1;
    int matrix = 1;
    bool fullRange = false;
};

struct Mp4AudioInfo {
    // AAC-LC frames (with their AudioSpecificConfig) or Opus packets.
    bool aac = false;
    std::vector<uint8_t> aacConfig;
    int bitrate = 0;
    int channels = 2;
    int sampleRate = 48000;
    int streams = 1;
    int coupledStreams = 1;
    uint8_t mapping[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    int samplesPerPacket = 240;
};

class Mp4Writer
{
public:
    Mp4Writer() = default;
    ~Mp4Writer();
    Mp4Writer(const Mp4Writer&) = delete;
    Mp4Writer& operator=(const Mp4Writer&) = delete;

    // Creates the file (never replaces one). Nothing is written until the first key frame.
    bool open(const std::wstring& path, const Mp4VideoInfo& video, const Mp4AudioInfo* audio);

    // One frame as the stream carries it (Annex B for H.264 and HEVC, OBUs for AV1). Times are microseconds on a
    // clock shared with the audio. Frames before the first key frame are skipped.
    bool addVideo(const uint8_t* data, size_t size, int64_t timeUs, bool key);

    // One Opus packet. Packets from before the first key frame are skipped.
    bool addAudio(const uint8_t* data, size_t size, int64_t timeUs);

    // Writes what is pending, the duration and the key frame index, and closes the file.
    bool close();

    bool started() const { return m_Started; }
    uint64_t bytes() const { return m_Offset; }
    int64_t durationMs() const;
    const std::string& error() const { return m_Error; }

private:
    struct Sample {
        uint32_t size;
        uint32_t duration;
        bool key;
    };
    struct KeyEntry {
        uint64_t time;
        uint64_t moofOffset;
    };

    bool fail(const char* what);
    bool writeBytes(const std::vector<uint8_t>& bytes);
    bool writeHeader(const uint8_t* keyFrame, size_t size);
    bool flushFragment();
    bool toSample(const uint8_t* data, size_t size, std::vector<uint8_t>& out);

    FILE* m_File = nullptr;
    std::string m_Error;
    Mp4VideoInfo m_Video;
    Mp4AudioInfo m_Audio;
    bool m_HasAudio = false;
    bool m_Started = false;
    bool m_Failed = false;

    uint64_t m_Offset = 0;
    uint64_t m_MehdOffset = 0;
    uint64_t m_MvhdDurationOffset = 0;
    uint32_t m_Sequence = 0;
    int64_t m_StartUs = 0;

    // Video: the fragment being built.
    std::vector<uint8_t> m_VideoData;
    std::vector<Sample> m_VideoSamples;
    uint64_t m_VideoFragmentStart = 0;  // decode time (90 kHz) of the first sample in the fragment
    uint64_t m_LastVideoTime = 0;       // decode time of the newest sample (its duration comes with the next one)
    uint64_t m_VideoEnd = 0;            // decode time at the end of everything flushed or pending with a duration
    uint32_t m_LastVideoDuration = 1500;
    bool m_HaveVideo = false;

    // Audio: the fragment being built.
    std::vector<uint8_t> m_AudioData;
    std::vector<Sample> m_AudioSamples;
    uint64_t m_AudioFragmentStart = 0;  // decode time (48 kHz) of the first sample in the fragment
    uint64_t m_AudioNext = 0;           // decode time the next packet would start at
    bool m_AudioAnchored = false;

    std::vector<KeyEntry> m_Keys;
};

}
