#include "aacencoder.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace GpuVM {

AacEncoder::~AacEncoder()
{
    close();
}

void AacEncoder::close()
{
#ifdef _WIN32
    if (m_Encoder != nullptr) {
        m_Encoder->Release();
        m_Encoder = nullptr;
        MFShutdown();
    }
    if (m_Com) {
        CoUninitialize();
        m_Com = false;
    }
#endif
}

bool AacEncoder::open(int channels, int sampleRate)
{
#ifdef _WIN32
    if (channels != 1 && channels != 2 && channels != 6) {
        return false;
    }
    m_Com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        close();
        return false;
    }
    MFT_REGISTER_TYPE_INFO input = { MFMediaType_Audio, MFAudioFormat_PCM };
    MFT_REGISTER_TYPE_INFO output = { MFMediaType_Audio, MFAudioFormat_AAC };
    IMFActivate** found = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                           &input, &output, &found, &count);
    if (SUCCEEDED(hr) && count > 0) {
        hr = found[0]->ActivateObject(IID_PPV_ARGS(&m_Encoder));
    }
    for (UINT32 i = 0; i < count; i++) {
        found[i]->Release();
    }
    CoTaskMemFree(found);
    if (m_Encoder == nullptr) {
        MFShutdown();
        m_Encoder = nullptr;
        if (m_Com) { CoUninitialize(); m_Com = false; }
        return false;
    }

    // 192 kb/s for stereo (the encoder's highest), less if a layout doesn't take it.
    const UINT32 rates[] = { 24000, 20000, 16000, 12000 };
    bool set = false;
    for (UINT32 bytesPerSecond : rates) {
        IMFMediaType* out = nullptr;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)sampleRate);
        out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels);
        out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytesPerSecond);
        out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);  // raw frames, as MP4 stores them
        out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        hr = m_Encoder->SetOutputType(0, out, 0);
        out->Release();
        if (SUCCEEDED(hr)) {
            m_Bitrate = (int)bytesPerSecond * 8;
            set = true;
            break;
        }
    }
    IMFMediaType* in = nullptr;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)sampleRate);
    in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels);
    in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32)channels * 2);
    in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(sampleRate * channels * 2));
    if (set) {
        set = SUCCEEDED(m_Encoder->SetInputType(0, in, 0));
    }
    in->Release();
    MFT_OUTPUT_STREAM_INFO info = {};
    if (!set || FAILED(m_Encoder->GetOutputStreamInfo(0, &info))) {
        close();
        return false;
    }
    m_OutputSize = info.cbSize ? info.cbSize : 8192;
    m_Encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_Encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    m_Started = true;
    m_Channels = channels;
    m_SampleRate = sampleRate;
    return true;
#else
    (void)channels;
    (void)sampleRate;
    return false;
#endif
}

bool AacEncoder::pull(std::vector<AacFrame>& out)
{
#ifdef _WIN32
    for (;;) {
        IMFSample* sample = nullptr;
        IMFMediaBuffer* buffer = nullptr;
        MFCreateSample(&sample);
        MFCreateMemoryBuffer(m_OutputSize, &buffer);
        sample->AddBuffer(buffer);
        MFT_OUTPUT_DATA_BUFFER data = {};
        data.pSample = sample;
        DWORD status = 0;
        HRESULT hr = m_Encoder->ProcessOutput(0, 1, &data, &status);
        if (data.pEvents) data.pEvents->Release();
        if (SUCCEEDED(hr)) {
            LONGLONG time = 0;
            sample->GetSampleTime(&time);
            BYTE* p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(buffer->Lock(&p, nullptr, &len))) {
                if (len > 0) {
                    out.push_back({ std::vector<uint8_t>(p, p + len), time / 10 });
                }
                buffer->Unlock();
            }
        }
        buffer->Release();
        sample->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return true;
        }
        if (FAILED(hr)) {
            return false;
        }
    }
#else
    (void)out;
    return false;
#endif
}

bool AacEncoder::encode(const int16_t* pcm, int frames, int64_t timeUs, std::vector<AacFrame>& out)
{
#ifdef _WIN32
    if (!m_Started || frames <= 0) {
        return m_Started;
    }
    DWORD bytes = (DWORD)frames * (DWORD)m_Channels * 2;
    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    MFCreateSample(&sample);
    MFCreateMemoryBuffer(bytes, &buffer);
    BYTE* p = nullptr;
    buffer->Lock(&p, nullptr, nullptr);
    memcpy(p, pcm, bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);
    sample->AddBuffer(buffer);
    sample->SetSampleTime(timeUs * 10);
    sample->SetSampleDuration((LONGLONG)frames * 10000000 / m_SampleRate);
    HRESULT hr = m_Encoder->ProcessInput(0, sample, 0);
    buffer->Release();
    sample->Release();
    if (hr == MF_E_NOTACCEPTING) {
        if (!pull(out)) return false;
        return encode(pcm, frames, timeUs, out);
    }
    if (FAILED(hr)) {
        return false;
    }
    return pull(out);
#else
    (void)pcm; (void)frames; (void)timeUs; (void)out;
    return false;
#endif
}

void AacEncoder::drain(std::vector<AacFrame>& out)
{
#ifdef _WIN32
    if (!m_Started) {
        return;
    }
    m_Encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    pull(out);
#else
    (void)out;
#endif
}

std::vector<uint8_t> AacEncoder::audioSpecificConfig() const
{
    // AAC-LC (2), the rate's index, the channel layout (1, 2 or 6 = 5.1).
    static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000 };
    int index = 3;
    for (int i = 0; i < 12; i++) {
        if (rates[i] == m_SampleRate) index = i;
    }
    int layout = m_Channels == 6 ? 6 : m_Channels;
    uint16_t asc = (uint16_t)((2 << 11) | (index << 7) | (layout << 3));
    return { (uint8_t)(asc >> 8), (uint8_t)asc };
}

}
