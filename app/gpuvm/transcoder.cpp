#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "transcoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <Limelight.h>

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace GpuVM {

namespace {

AVPixelFormat pickFormat(AVCodecContext*, const AVPixelFormat* formats)
{
    // The GPU's decoder when it is offered (D3D11VA, as the renderer uses), else the first software format.
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; f++) {
        if (*f == AV_PIX_FMT_D3D11) {
            return *f;
        }
    }
    return formats[0];
}

// Start code + NAL type of each NAL in an Annex B buffer.
bool hasSps(const std::vector<uint8_t>& s)
{
    for (size_t i = 0; i + 3 < s.size(); i++) {
        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
            if ((s[i + 3] & 0x1f) == 7) {
                return true;
            }
            i += 2;
        }
    }
    return false;
}

#ifdef _WIN32
bool setCodecValue(IMFTransform* mft, const GUID& key, UINT32 value)
{
    ICodecAPI* api = nullptr;
    if (FAILED(mft->QueryInterface(__uuidof(ICodecAPI), (void**)&api))) {
        return false;
    }
    VARIANT v = {};
    v.vt = VT_UI4;
    v.ulVal = value;
    bool ok = SUCCEEDED(api->SetValue(&key, &v));
    api->Release();
    return ok;
}
#endif

}

Transcoder::~Transcoder()
{
    close();
}

bool Transcoder::supported(int videoFormat)
{
    return (videoFormat & (VIDEO_FORMAT_MASK_10BIT | VIDEO_FORMAT_MASK_YUV444)) == 0;
}

void Transcoder::close()
{
    avcodec_free_context(&m_Decoder);
    av_buffer_unref(&m_HwDevice);
    av_packet_free(&m_Packet);
    av_frame_free(&m_Frame);
    av_frame_free(&m_SwFrame);
#ifdef _WIN32
    if (m_Encoder != nullptr) {
        m_Encoder->Release();
        m_Encoder = nullptr;
    }
    if (m_Mf) {
        MFShutdown();
        m_Mf = false;
    }
    if (m_Com) {
        CoUninitialize();
        m_Com = false;
    }
#endif
}

bool Transcoder::open(VideoCodec codec, int width, int height, const Settings& settings)
{
    m_Settings = settings;
    m_Settings.fps = std::clamp(m_Settings.fps, 1, 240);
    m_Width = width;
    m_Height = height;
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        m_Error = "size";
        return false;
    }
    m_Packet = av_packet_alloc();
    m_Frame = av_frame_alloc();
    m_SwFrame = av_frame_alloc();
    if (!m_Packet || !m_Frame || !m_SwFrame || !openDecoder(codec) || !openEncoder()) {
        if (m_Error.empty()) m_Error = "open";
        close();
        return false;
    }
    return true;
}

bool Transcoder::openDecoder(VideoCodec codec)
{
    bool hw = av_hwdevice_ctx_create(&m_HwDevice, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) == 0;
    const AVCodec* decoder = nullptr;
    switch (codec) {
    case VideoCodec::H264: decoder = avcodec_find_decoder(AV_CODEC_ID_H264); break;
    case VideoCodec::HEVC: decoder = avcodec_find_decoder(AV_CODEC_ID_HEVC); break;
    // FFmpeg's own AV1 decoder only decodes on the GPU; dav1d on the CPU otherwise.
    case VideoCodec::AV1: decoder = avcodec_find_decoder_by_name(hw ? "av1" : "libdav1d"); break;
    }
    if (decoder == nullptr) {
        m_Error = "no-decoder";
        return false;
    }
    m_Decoder = avcodec_alloc_context3(decoder);
    if (m_Decoder == nullptr) {
        return false;
    }
    m_Decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (hw) {
        m_Decoder->hw_device_ctx = av_buffer_ref(m_HwDevice);
        m_Decoder->get_format = pickFormat;
    }
    else {
        m_Decoder->thread_count = 4;
    }
    if (avcodec_open2(m_Decoder, decoder, nullptr) < 0) {
        m_Error = "decoder";
        return false;
    }
    return true;
}

bool Transcoder::openEncoder()
{
#ifdef _WIN32
    m_Com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        m_Error = "mf";
        return false;
    }
    m_Mf = true;
    // Windows' own H.264 encoder (synchronous, on the CPU). Hardware encoders are left out on purpose: the GPU's
    // encoder makes the machines' live pictures (GPU-P shares it), and a recording must never take from them.
    MFT_REGISTER_TYPE_INFO input = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO output = { MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** found = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                           &input, &output, &found, &count);
    if (SUCCEEDED(hr) && count > 0) {
        found[0]->ActivateObject(IID_PPV_ARGS(&m_Encoder));
    }
    for (UINT32 i = 0; i < count; i++) {
        found[i]->Release();
    }
    CoTaskMemFree(found);
    if (m_Encoder == nullptr) {
        m_Error = "no-encoder";
        return false;
    }
    // A desktop: mostly still, sharp text. Variable bitrate around the target, no B-frames (the MP4 writer stores
    // frames in decoding order with no reordering), a key frame every few seconds for seeking.
    if (m_Settings.quality > 0) {
        setCodecValue(m_Encoder, CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality);
        setCodecValue(m_Encoder, CODECAPI_AVEncCommonQuality, (UINT32)std::min(m_Settings.quality, 100));
    }
    else {
        setCodecValue(m_Encoder, CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_UnconstrainedVBR);
        setCodecValue(m_Encoder, CODECAPI_AVEncCommonMeanBitRate, (UINT32)m_Settings.bitrate);
    }
    setCodecValue(m_Encoder, CODECAPI_AVEncMPVGOPSize, (UINT32)(m_Settings.fps * m_Settings.keySeconds));
    setCodecValue(m_Encoder, CODECAPI_AVEncMPVDefaultBPictureCount, 0);

    IMFMediaType* out = nullptr;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)m_Settings.bitrate);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    MFSetAttributeSize(out, MF_MT_FRAME_SIZE, (UINT32)m_Width, (UINT32)m_Height);
    MFSetAttributeRatio(out, MF_MT_FRAME_RATE, (UINT32)m_Settings.fps, 1);
    MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = m_Encoder->SetOutputType(0, out, 0);
    if (FAILED(hr)) {
        // An encoder without High: Main.
        out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        hr = m_Encoder->SetOutputType(0, out, 0);
    }
    out->Release();
    if (FAILED(hr)) {
        m_Error = "encoder-output";
        return false;
    }
    IMFMediaType* in = nullptr;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in, MF_MT_FRAME_SIZE, (UINT32)m_Width, (UINT32)m_Height);
    MFSetAttributeRatio(in, MF_MT_FRAME_RATE, (UINT32)m_Settings.fps, 1);
    MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = m_Encoder->SetInputType(0, in, 0);
    in->Release();
    if (FAILED(hr)) {
        m_Error = "encoder-input";
        return false;
    }
    MFT_OUTPUT_STREAM_INFO info = {};
    if (FAILED(m_Encoder->GetOutputStreamInfo(0, &info))) {
        m_Error = "encoder-info";
        return false;
    }
    m_EncoderProvidesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    m_OutputSize = std::max<unsigned>(info.cbSize, (unsigned)(m_Width * m_Height * 3 / 2));
    // The parameter sets, for a key frame that comes without them.
    IMFMediaType* current = nullptr;
    if (SUCCEEDED(m_Encoder->GetOutputCurrentType(0, &current))) {
        UINT32 size = 0;
        if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0) {
            m_SequenceHeader.resize(size);
            current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, m_SequenceHeader.data(), size, nullptr);
        }
        current->Release();
    }
    m_Encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_Encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
#else
    m_Error = "no-encoder";
    return false;
#endif
}

bool Transcoder::push(const uint8_t* data, size_t size, int64_t timeUs, bool key, std::vector<EncodedFrame>& out)
{
    if (m_Decoder == nullptr) {
        return false;
    }
    m_Counts.in++;
    if (!m_SawKey && !key) {
        return true;
    }
    m_SawKey = true;
    av_packet_unref(m_Packet);
    if (av_new_packet(m_Packet, (int)size) < 0) {
        return true;
    }
    memcpy(m_Packet->data, data, size);
    m_Packet->pts = timeUs;
    m_Packet->dts = timeUs;
    if (key) {
        m_Packet->flags |= AV_PKT_FLAG_KEY;
    }
    int ret = avcodec_send_packet(m_Decoder, m_Packet);
    if (ret == AVERROR(EAGAIN)) {
        // The decoder is full: take its frames, then give it this one again.
        if (!receiveFrames(out)) return false;
        ret = avcodec_send_packet(m_Decoder, m_Packet);
    }
    if (ret < 0) {
        m_Counts.badPackets++;
    }
    return receiveFrames(out);
}

bool Transcoder::receiveFrames(std::vector<EncodedFrame>& out)
{
    for (;;) {
        int ret = avcodec_receive_frame(m_Decoder, m_Frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }
        if (ret < 0) {
            m_Counts.badPackets++;
            return true;
        }
        m_Counts.decoded++;
        int64_t t = m_Frame->best_effort_timestamp != AV_NOPTS_VALUE ? m_Frame->best_effort_timestamp : m_Frame->pts;
        const int64_t interval = 1'000'000 / m_Settings.fps;
        bool keep = m_NextUs == INT64_MIN || t >= m_NextUs;
        if (keep) {
            // Evenly spaced on the stream's clock; after a pause (a still screen sends nothing) it starts again from
            // the frame that broke it.
            // Never two kept frames closer than half an interval: a late frame doesn't drag the next slot onto it
            // (lab 061: frames 5 ms apart after a late one, with the same time once rounded).
            m_NextUs = (m_NextUs == INT64_MIN || t - m_NextUs > interval) ? t + interval : m_NextUs + interval;
            while (m_NextUs < t + interval / 2) {
                m_NextUs += interval;
            }
            m_Counts.kept++;
            if (!encode(m_Frame, t, out)) {
                av_frame_unref(m_Frame);
                return false;
            }
        }
        av_frame_unref(m_Frame);
    }
}

bool Transcoder::encode(const AVFrame* frame, int64_t timeUs, std::vector<EncodedFrame>& out)
{
#ifdef _WIN32
    const AVFrame* src = frame;
    if (frame->format == AV_PIX_FMT_D3D11) {
        // Only the frames kept leave the GPU.
        av_frame_unref(m_SwFrame);
        m_SwFrame->format = AV_PIX_FMT_NV12;
        if (av_hwframe_transfer_data(m_SwFrame, frame, 0) < 0) {
            m_Counts.badPackets++;
            return true;
        }
        src = m_SwFrame;
    }
    if (src->width != m_Width || src->height != m_Height) {
        return true;  // the stream changed size: the caller starts a new file
    }
    bool nv12 = src->format == AV_PIX_FMT_NV12;
    bool planar = src->format == AV_PIX_FMT_YUV420P || src->format == AV_PIX_FMT_YUVJ420P;
    if (!nv12 && !planar) {
        const char* name = av_get_pix_fmt_name((AVPixelFormat)src->format);
        m_Error = std::string("format ") + (name ? name : "?");
        return true;
    }
    const DWORD lumaBytes = (DWORD)(m_Width * m_Height);
    const DWORD bytes = lumaBytes * 3 / 2;
    IMFMediaBuffer* buffer = nullptr;
    if (FAILED(MFCreateMemoryBuffer(bytes, &buffer))) {
        m_Error = "memory";
        return false;
    }
    BYTE* p = nullptr;
    buffer->Lock(&p, nullptr, nullptr);
    for (int y = 0; y < m_Height; y++) {
        memcpy(p + (size_t)y * m_Width, src->data[0] + (size_t)y * src->linesize[0], (size_t)m_Width);
    }
    BYTE* uv = p + lumaBytes;
    const int ch = m_Height / 2, cw = m_Width / 2;
    for (int y = 0; y < ch; y++) {
        BYTE* row = uv + (size_t)y * m_Width;
        if (nv12) {
            memcpy(row, src->data[1] + (size_t)y * src->linesize[1], (size_t)m_Width);
        }
        else {
            const uint8_t* u = src->data[1] + (size_t)y * src->linesize[1];
            const uint8_t* v = src->data[2] + (size_t)y * src->linesize[2];
            for (int x = 0; x < cw; x++) {
                row[x * 2] = u[x];
                row[x * 2 + 1] = v[x];
            }
        }
    }
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);
    IMFSample* sample = nullptr;
    MFCreateSample(&sample);
    sample->AddBuffer(buffer);
    buffer->Release();
    sample->SetSampleTime(timeUs * 10);
    sample->SetSampleDuration(10'000'000 / m_Settings.fps);
    HRESULT hr = m_Encoder->ProcessInput(0, sample, 0);
    if (hr == MF_E_NOTACCEPTING) {
        if (pull(out)) {
            hr = m_Encoder->ProcessInput(0, sample, 0);
        }
    }
    sample->Release();
    if (FAILED(hr)) {
        m_Error = "encode";
        return false;
    }
    return pull(out);
#else
    (void)frame; (void)timeUs; (void)out;
    return false;
#endif
}

bool Transcoder::pull(std::vector<EncodedFrame>& out)
{
#ifdef _WIN32
    for (;;) {
        IMFSample* sample = nullptr;
        if (!m_EncoderProvidesSamples) {
            IMFMediaBuffer* buffer = nullptr;
            MFCreateSample(&sample);
            MFCreateMemoryBuffer(m_OutputSize, &buffer);
            sample->AddBuffer(buffer);
            buffer->Release();
        }
        MFT_OUTPUT_DATA_BUFFER data = {};
        data.pSample = sample;
        DWORD status = 0;
        HRESULT hr = m_Encoder->ProcessOutput(0, 1, &data, &status);
        if (data.pEvents) data.pEvents->Release();
        if (SUCCEEDED(hr) && data.pSample != nullptr) {
            EncodedFrame f;
            LONGLONG time = 0;
            data.pSample->GetSampleTime(&time);
            f.timeUs = time / 10;
            f.key = MFGetAttributeUINT32(data.pSample, MFSampleExtension_CleanPoint, FALSE) != 0;
            IMFMediaBuffer* contiguous = nullptr;
            if (SUCCEEDED(data.pSample->ConvertToContiguousBuffer(&contiguous))) {
                BYTE* p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(contiguous->Lock(&p, nullptr, &len))) {
                    f.data.assign(p, p + len);
                    contiguous->Unlock();
                }
                contiguous->Release();
            }
            if (f.key && !hasSps(f.data) && !m_SequenceHeader.empty()) {
                f.data.insert(f.data.begin(), m_SequenceHeader.begin(), m_SequenceHeader.end());
            }
            if (!f.data.empty()) {
                m_Counts.encoded++;
                m_Counts.keys += f.key ? 1 : 0;
                m_Counts.bytes += (int64_t)f.data.size();
                out.push_back(std::move(f));
            }
        }
        if (data.pSample != nullptr && data.pSample != sample) {
            data.pSample->Release();
        }
        if (sample != nullptr) {
            sample->Release();
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return true;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // The encoder settled its output format: take it as it offers it, and go on.
            IMFMediaType* type = nullptr;
            if (FAILED(m_Encoder->GetOutputAvailableType(0, 0, &type))) return false;
            hr = m_Encoder->SetOutputType(0, type, 0);
            type->Release();
            if (FAILED(hr)) return false;
            continue;
        }
        if (FAILED(hr)) {
            m_Error = "encode-output";
            return false;
        }
    }
#else
    (void)out;
    return false;
#endif
}

void Transcoder::drain(std::vector<EncodedFrame>& out)
{
    if (m_Decoder == nullptr) {
        return;
    }
    if (avcodec_send_packet(m_Decoder, nullptr) == 0) {
        receiveFrames(out);
    }
#ifdef _WIN32
    if (m_Encoder != nullptr) {
        m_Encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_Encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        pull(out);
    }
#endif
}

std::string Transcoder::summary() const
{
    char line[256];
    char rate[32];
    if (m_Settings.quality > 0) snprintf(rate, sizeof rate, "quality %d", m_Settings.quality);
    else snprintf(rate, sizeof rate, "%dkb/s", m_Settings.bitrate / 1000);
    snprintf(line, sizeof line, "light %dfps %s, %s decode: %d in, %d decoded, %d bad, %d kept, %d encoded (%d key), %lld KB%s%s",
             m_Settings.fps, rate, m_HwDevice ? "GPU" : "CPU", m_Counts.in, m_Counts.decoded,
             m_Counts.badPackets, m_Counts.kept, m_Counts.encoded, m_Counts.keys, (long long)(m_Counts.bytes / 1024),
             m_Error.empty() ? "" : ", ", m_Error.c_str());
    return line;
}

}
