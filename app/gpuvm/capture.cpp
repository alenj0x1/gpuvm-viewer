#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "capture.h"
#include "audiotrack.h"
#include "bridge.h"
#include "mp4writer.h"
#include "viewport.h"

#include "streaming/session.h"

#include <Limelight.h>

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#undef min
#undef max

namespace GpuVM {

namespace {

struct Packet {
    bool video = false;
    bool key = false;
    bool split = false;  // the stream's format changed: the recording goes on in a new file
    int64_t timeUs = 0;
    std::shared_ptr<const std::vector<uint8_t>> data;
};

struct Segment {
    int64_t startUs = 0;
    size_t bytes = 0;
    std::vector<Packet> packets;
};

// While recording or keeping the last seconds, a key frame is asked for when the stream hasn't sent one for this
// long (Sunshine sends them only on request): files can be seeked, and the buffer can start that close to the end.
constexpr int64_t kKeyIntervalUs = 10'000'000;
constexpr int64_t kIdrRequestGapUs = 2'000'000;
// The replay buffer never holds more than this (at 35 Mb/s, 30 s are ~131 MB; at 150 Mb/s it keeps fewer seconds).
constexpr size_t kReplayCapBytes = 320u << 20;
// A recording whose disk can't keep up stops instead of piling the stream up in memory.
constexpr size_t kRecordQueueCapBytes = 256u << 20;
constexpr uint64_t kMinFreeToStart = 1ull << 30;
constexpr uint64_t kMinFreeWhileRecording = 512ull << 20;

QString capturesDir()
{
    return Bridge::dataDirectory() + "/Captures";
}

QString newName(const char* prefix, const char* ext)
{
    static std::atomic<int> counter{0};
    return QString("%1-%2-%3.%4").arg(QLatin1String(prefix),
                                       QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"),
                                       QString::number(++counter % 10000), QLatin1String(ext));
}

uint64_t freeBytes(const QString& dir)
{
#ifdef Q_OS_WIN32
    ULARGE_INTEGER available = {};
    if (GetDiskFreeSpaceExW((const wchar_t*)QDir::toNativeSeparators(dir).utf16(), &available, nullptr, nullptr)) {
        return available.QuadPart;
    }
#else
    Q_UNUSED(dir);
#endif
    return UINT64_MAX;
}

// Moves a finished .part file to its name and tells the app it is ready.
bool publish(const QString& part, const QString& name, QJsonObject fields)
{
    QString final = capturesDir() + "/" + name;
    if (!QFile::rename(part, final)) {
        QFile::remove(part);
        return false;
    }
    fields["file"] = name;
    fields["bytes"] = (double)QFileInfo(final).size();
    Bridge::instance().send("capture", fields);
    return true;
}

void reportError(const char* kind, const char* reason)
{
    QJsonObject f;
    f["kind"] = kind;
    f["reason"] = reason;
    Bridge::instance().send("capture-error", f);
}

// Short notices over the picture (never in the files): Spanish or English, as the app is.
const char* text(const QString& lang, const char* key)
{
    struct Entry { const char* key; const char* es; const char* en; };
    static const Entry entries[] = {
        { "shot", "Captura guardada", "Screenshot saved" },
        { "recording", "Grabación guardada", "Recording saved" },
        { "replay", "Clip guardado", "Clip saved" },
        { "replay-off", "Activa los últimos 30 s en Ajustes", "Turn on the last 30 s in Settings" },
        { "disk-full", "No hay espacio en el disco", "The disk is full" },
        { "error", "No se pudo guardar", "Couldn't save" },
    };
    for (const Entry& e : entries) {
        if (strcmp(e.key, key) == 0) {
            return lang == QLatin1String("en") ? e.en : e.es;
        }
    }
    return "";
}

// --- Screenshot: the decoded frame to an sRGB picture ------------------------------------------------------------

float pqToLinear(float e)
{
    // SMPTE ST 2084 EOTF, in units of SDR reference white (203 cd/m2).
    const float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 4096.0f * 128.0f;
    const float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 4096.0f * 32.0f, c3 = 2392.0f / 4096.0f * 32.0f;
    float p = std::pow(std::max(e, 0.0f), 1.0f / m2);
    float l = std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
    return l * 10000.0f / 203.0f;
}

uint8_t srgb(float linear)
{
    linear = std::clamp(linear, 0.0f, 1.0f);
    float v = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return (uint8_t)std::lround(v * 255.0f);
}

// The picture in sRGB; with step > 1, only every step-th pixel (a quick small copy for a thumbnail).
QImage toImage(const FramePlanes& f, int step = 1)
{
    QImage img(f.width / step, f.height / step, QImage::Format_RGB888);
    if (img.isNull()) {
        return img;
    }
    double kr = 0.299, kb = 0.114;
    if (f.colorspace == 1) { kr = 0.2126; kb = 0.0722; }               // BT.709
    else if (f.colorspace == 9 || f.colorspace == 10) { kr = 0.2627; kb = 0.0593; }  // BT.2020
    const float kg = (float)(1.0 - kr - kb);
    const float crR = (float)(2.0 * (1.0 - kr)), cbB = (float)(2.0 * (1.0 - kb));
    const float s = f.tenBit ? 4.0f : 1.0f;
    const float maxCode = f.tenBit ? 1023.0f : 255.0f;
    const float yOff = f.fullRange ? 0.0f : 16.0f * s, yScale = f.fullRange ? maxCode : 219.0f * s;
    const float cOff = 128.0f * s, cScale = f.fullRange ? maxCode : 224.0f * s;
    const int bps = f.tenBit ? 2 : 1;
    const size_t lumaPitch = (size_t)f.width * bps;
    const size_t chromaPitch = (size_t)((f.width + 1) / 2) * 2 * bps;
    const int cw = (f.width + 1) / 2, ch = (f.height + 1) / 2;

    auto sample = [&](const std::vector<uint8_t>& plane, size_t offset) -> float {
        if (bps == 1) return plane[offset];
        uint16_t v = (uint16_t)(plane[offset] | (plane[offset + 1] << 8));
        return (float)(v >> 6);
    };

    // HDR10: PQ to linear through a table, BT.2020 to BT.709, a soft knee for highlights, then sRGB.
    std::vector<float> pqTable;
    std::vector<uint8_t> srgbTable;
    if (f.pq) {
        pqTable.resize(1024);
        for (int i = 0; i < 1024; i++) pqTable[i] = pqToLinear(i / 1023.0f);
        srgbTable.resize(4096);
        for (int i = 0; i < 4096; i++) srgbTable[i] = srgb(i / 4095.0f);
    }

    for (int oy = 0; oy < img.height(); oy++) {
        uchar* out = img.scanLine(oy);
        int y = oy * step;
        // Chroma sits halfway between two rows of luma (4:2:0, left-aligned horizontally).
        float cyf = std::clamp((y - 0.5f) / 2.0f, 0.0f, (float)(ch - 1));
        int cy0 = (int)cyf, cy1 = std::min(cy0 + 1, ch - 1);
        float wy = cyf - cy0;
        for (int ox = 0; ox < img.width(); ox++) {
            int x = ox * step;
            float Y = (sample(f.luma, (size_t)y * lumaPitch + (size_t)x * bps) - yOff) / yScale;
            float cxf = std::min(x / 2.0f, (float)(cw - 1));
            int cx0 = (int)cxf, cx1 = std::min(cx0 + 1, cw - 1);
            float wx = cxf - cx0;
            auto chroma = [&](int k) {
                float a = sample(f.chroma, (size_t)cy0 * chromaPitch + ((size_t)cx0 * 2 + k) * bps);
                float b = sample(f.chroma, (size_t)cy0 * chromaPitch + ((size_t)cx1 * 2 + k) * bps);
                float c = sample(f.chroma, (size_t)cy1 * chromaPitch + ((size_t)cx0 * 2 + k) * bps);
                float d = sample(f.chroma, (size_t)cy1 * chromaPitch + ((size_t)cx1 * 2 + k) * bps);
                float top = a + (b - a) * wx, bottom = c + (d - c) * wx;
                return (top + (bottom - top) * wy - cOff) / cScale;
            };
            float Cb = chroma(0), Cr = chroma(1);
            float R = Y + crR * Cr, B = Y + cbB * Cb;
            float G = (Y - (float)kr * R - (float)kb * B) / kg;
            if (!f.pq) {
                out[ox * 3] = (uchar)std::lround(std::clamp(R, 0.0f, 1.0f) * 255.0f);
                out[ox * 3 + 1] = (uchar)std::lround(std::clamp(G, 0.0f, 1.0f) * 255.0f);
                out[ox * 3 + 2] = (uchar)std::lround(std::clamp(B, 0.0f, 1.0f) * 255.0f);
                continue;
            }
            float r = pqTable[(int)std::lround(std::clamp(R, 0.0f, 1.0f) * 1023.0f)];
            float g = pqTable[(int)std::lround(std::clamp(G, 0.0f, 1.0f) * 1023.0f)];
            float b = pqTable[(int)std::lround(std::clamp(B, 0.0f, 1.0f) * 1023.0f)];
            float r7 = 1.6605f * r - 0.5876f * g - 0.0728f * b;
            float g7 = -0.1246f * r + 1.1329f * g - 0.0083f * b;
            float b7 = -0.0182f * r - 0.1006f * g + 1.1187f * b;
            float lum = std::max(0.2126f * r7 + 0.7152f * g7 + 0.0722f * b7, 1e-6f);
            // Up to white as it is; above it, compressed so a 1000-nit highlight lands just under white.
            float mapped = lum <= 0.8f ? lum : 0.8f + 0.2f * (1.0f - std::exp(-(lum - 0.8f) / 0.8f));
            float k = mapped / lum;
            auto enc = [&](float v) { return srgbTable[(int)std::lround(std::clamp(v * k, 0.0f, 1.0f) * 4095.0f)]; };
            out[ox * 3] = enc(r7);
            out[ox * 3 + 1] = enc(g7);
            out[ox * 3 + 2] = enc(b7);
        }
    }
    return img;
}

// A small JPEG as a data URL for the panel (the pipe takes lines up to 16 KB).
QString thumbnailOf(const QImage& image)
{
    if (image.isNull()) {
        return QString();
    }
    QImage small = image.scaledToWidth(192, Qt::SmoothTransformation);
    for (int quality : { 72, 55, 40 }) {
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        if (small.save(&buffer, "JPG", quality) && jpeg.size() <= 9000) {
            return QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(jpeg.toBase64());
        }
    }
    return QString();
}

}

struct Capture::Impl {
    std::mutex lock;

    // The stream (lock).
    Mp4VideoInfo video;
    bool haveVideo = false;
    Mp4AudioInfo opus;
    bool haveAudio = false;
    int64_t clockOffset = INT64_MAX;  // receive clock minus the host's presentation clock (smallest seen)
    uint64_t lastKeyUs = 0;           // receive clock
    uint64_t lastIdrRequestUs = 0;
    QString lang = "es";

    // Replay buffer (lock): segments that each start at a key frame.
    int replaySeconds = 30;
    std::deque<Segment> ring;
    size_t ringBytes = 0;
    std::thread saver;
    std::atomic<bool> saving{false};

    // Recording (lock for the queue).
    std::thread recorder;
    std::condition_variable wake;
    std::deque<Packet> queue;
    size_t queueBytes = 0;
    bool stopRecording = false;
    Uint32 recordingSince = 0;

    // Screenshot.
    std::thread shooter;
    SDL_TimerID shotTimer = 0;

    // The newest thumbnail (thumbLock): taken when a recording starts or a clip is saved, sent with the file.
    std::mutex thumbLock;
    std::condition_variable thumbReady;
    QString thumb;
    unsigned thumbSeq = 0;
    std::thread thumber;

    // The notice over the picture (overlayLock).
    std::mutex overlayLock;
    QString notice;
    Uint32 noticeUntil = 0;
    SDL_TimerID overlayTimer = 0;
    QString shownText;
};

Capture& Capture::instance()
{
    static Capture capture;
    return capture;
}

Capture::Impl* Capture::impl()
{
    // Created on first use and kept for the process (the viewer runs one session).
    static Impl impl;
    return &impl;
}

// --- Notices over the picture ------------------------------------------------------------------------------------

namespace {

// Recomputes what the overlay shows: a notice for a moment, else the recording's time, else nothing. Any thread.
void refreshOverlay(Capture::Impl* d, bool recording)
{
    QString textNow;
    {
        std::lock_guard<std::mutex> g(d->overlayLock);
        Uint32 now = SDL_GetTicks();
        if (!d->notice.isEmpty() && !SDL_TICKS_PASSED(now, d->noticeUntil)) {
            textNow = d->notice;
        }
        else if (recording) {
            Uint32 s = (now - d->recordingSince) / 1000;
            // \x01: the overlay draws the red recording dot before the time.
            textNow = QString("\x01%1:%2").arg(s / 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
            if (s >= 3600) {
                textNow = QString("\x01%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
            }
        }
        if (textNow == d->shownText) {
            return;
        }
        d->shownText = textNow;
    }
    Session* session = Session::get();
    if (session == nullptr) {
        return;
    }
    Overlay::OverlayManager& overlays = session->getOverlayManager();
    if (textNow.isEmpty()) {
        overlays.setOverlayState(Overlay::OverlayGpuVMCapture, false);
    }
    else {
        overlays.updateOverlayText(Overlay::OverlayGpuVMCapture, textNow.toUtf8().constData());
        overlays.setOverlayState(Overlay::OverlayGpuVMCapture, true);
    }
}

}

// The timer that keeps the overlay current runs only while something is shown.
struct OverlayTimer {
    static Uint32 tick(Uint32 interval, void* param)
    {
        Capture::Impl* d = (Capture::Impl*)param;
        bool recording = Capture::instance().m_Recording.load();
        refreshOverlay(d, recording);
        std::lock_guard<std::mutex> g(d->overlayLock);
        if (d->shownText.isEmpty()) {
            d->overlayTimer = 0;
            return 0;
        }
        return interval;
    }
};

static void ensureOverlayTimer(Capture::Impl* d)
{
    std::lock_guard<std::mutex> g(d->overlayLock);
    if (d->overlayTimer == 0) {
        d->overlayTimer = SDL_AddTimer(250, OverlayTimer::tick, d);
    }
}

static void showNotice(Capture::Impl* d, const char* key)
{
    QString lang;
    {
        std::lock_guard<std::mutex> g(d->lock);
        lang = d->lang;
    }
    {
        std::lock_guard<std::mutex> g(d->overlayLock);
        d->notice = QString::fromUtf8(text(lang, key));
        d->noticeUntil = SDL_GetTicks() + 2000;
    }
    refreshOverlay(d, Capture::instance().m_Recording.load());
    ensureOverlayTimer(d);
}

// --- Stream ------------------------------------------------------------------------------------------------------

void Capture::setVideoFormat(int videoFormat, int width, int height, int)
{
    Impl* d = impl();
    Mp4VideoInfo v;
    v.codec = (videoFormat & VIDEO_FORMAT_MASK_AV1) ? VideoCodec::AV1
              : (videoFormat & VIDEO_FORMAT_MASK_H265) ? VideoCodec::HEVC : VideoCodec::H264;
    v.width = width;
    v.height = height;
    bool changed;
    {
        std::lock_guard<std::mutex> g(d->lock);
        changed = d->haveVideo && (d->video.codec != v.codec || d->video.width != v.width || d->video.height != v.height);
        // Color is set from the frames themselves (onVideoFrame).
        v.primaries = d->video.primaries;
        v.transfer = d->video.transfer;
        v.matrix = d->video.matrix;
        v.fullRange = d->video.fullRange;
        d->video = v;
        d->haveVideo = true;
        if (changed) {
            // Older packets can't go in the same file as the new format.
            d->ring.clear();
            d->ringBytes = 0;
            m_ReplayBytes = 0;
            if (m_Recording) {
                Packet split;
                split.split = true;
                d->queue.push_back(split);
                d->wake.notify_one();
            }
        }
    }
}

void Capture::setAudioConfig(const _OPUS_MULTISTREAM_CONFIGURATION* config)
{
    Impl* d = impl();
    const OPUS_MULTISTREAM_CONFIGURATION* c = (const OPUS_MULTISTREAM_CONFIGURATION*)config;
    std::lock_guard<std::mutex> g(d->lock);
    d->opus = Mp4AudioInfo();
    d->opus.channels = c->channelCount;
    d->opus.sampleRate = c->sampleRate;
    d->opus.streams = c->streams;
    d->opus.coupledStreams = c->coupledStreams;
    d->opus.samplesPerPacket = c->samplesPerFrame;
    for (int i = 0; i < 8 && i < c->channelCount; i++) {
        d->opus.mapping[i] = c->mapping[i];
    }
    d->haveAudio = c->channelCount > 0;
}

void Capture::onVideoFrame(const _DECODE_UNIT* unit)
{
    bool recording = m_Recording.load(std::memory_order_relaxed), replay = m_Replay.load(std::memory_order_relaxed);
    if (!recording && !replay) {
        return;
    }
    const DECODE_UNIT* du = (const DECODE_UNIT*)unit;
    Impl* d = impl();
    auto data = std::make_shared<std::vector<uint8_t>>();
    data->reserve((size_t)du->fullLength);
    for (PLENTRY e = du->bufferList; e != nullptr; e = e->next) {
        data->insert(data->end(), (const uint8_t*)e->data, (const uint8_t*)e->data + e->length);
    }
    Packet p;
    p.video = true;
    p.key = du->frameType == FRAME_TYPE_IDR;
    p.timeUs = (int64_t)du->presentationTimeUs;
    p.data = data;

    bool askKey = false, tooSlow = false;
    {
        std::lock_guard<std::mutex> g(d->lock);
        d->clockOffset = std::min(d->clockOffset, (int64_t)du->receiveTimeUs - (int64_t)du->presentationTimeUs);
        if (p.key) {
            d->lastKeyUs = du->receiveTimeUs;
            // The picture's color as the stream says it (HDR10 or SDR, BT.601/709/2020).
            bool hdr = du->hdrActive;
            d->video.primaries = hdr || du->colorspace == COLORSPACE_REC_2020 ? 9 : du->colorspace == COLORSPACE_REC_601 ? 6 : 1;
            d->video.transfer = hdr ? 16 : du->colorspace == COLORSPACE_REC_601 ? 6 : 1;
            d->video.matrix = hdr || du->colorspace == COLORSPACE_REC_2020 ? 9 : du->colorspace == COLORSPACE_REC_601 ? 6 : 1;
        }
        if (d->lastKeyUs == 0 || du->receiveTimeUs - d->lastKeyUs > (uint64_t)kKeyIntervalUs) {
            if (du->receiveTimeUs - d->lastIdrRequestUs > (uint64_t)kIdrRequestGapUs) {
                d->lastIdrRequestUs = du->receiveTimeUs;
                askKey = true;
            }
        }
        if (recording) {
            if (d->queueBytes + data->size() > kRecordQueueCapBytes) {
                tooSlow = true;
            }
            else {
                d->queue.push_back(p);
                d->queueBytes += data->size();
                d->wake.notify_one();
            }
        }
        if (replay) {
            if (p.key) {
                d->ring.push_back(Segment{ p.timeUs, 0, {} });
            }
            if (!d->ring.empty()) {
                Segment& s = d->ring.back();
                s.packets.push_back(p);
                s.bytes += data->size();
                d->ringBytes += data->size();
                int64_t window = (int64_t)d->replaySeconds * 1000000;
                while (d->ring.size() >= 2 && d->ring[1].startUs <= p.timeUs - window) {
                    d->ringBytes -= d->ring.front().bytes;
                    d->ring.pop_front();
                }
                while (d->ring.size() >= 2 && d->ringBytes > kReplayCapBytes) {
                    d->ringBytes -= d->ring.front().bytes;
                    d->ring.pop_front();
                }
                if (d->ringBytes > kReplayCapBytes) {
                    // One key frame's worth over the cap: start again at the next one.
                    d->ring.clear();
                    d->ringBytes = 0;
                    askKey = true;
                }
                m_ReplayBytes = d->ringBytes;
            }
        }
    }
    if (askKey) {
        LiRequestIdrFrame();
    }
    if (tooSlow && m_Recording.exchange(false)) {
        // The disk can't keep up: the recording stops and keeps what it wrote.
        reportError("recording", "too-slow");
        {
            std::lock_guard<std::mutex> g(d->lock);
            d->stopRecording = true;
            d->wake.notify_one();
        }
        QJsonObject f;
        f["on"] = false;
        f["reason"] = "too-slow";
        Bridge::instance().send("recording", f);
    }
}

void Capture::onAudioPacket(const char* data, int length)
{
    bool recording = m_Recording.load(std::memory_order_relaxed), replay = m_Replay.load(std::memory_order_relaxed);
    if ((!recording && !replay) || length <= 0) {
        return;
    }
    Impl* d = impl();
    Packet p;
    p.data = std::make_shared<std::vector<uint8_t>>((const uint8_t*)data, (const uint8_t*)data + length);
    std::lock_guard<std::mutex> g(d->lock);
    if (d->clockOffset == INT64_MAX) {
        return;  // no picture yet: nothing to put it next to
    }
    // On the picture's clock: when it arrived, less the usual delay of the picture from the host to here.
    p.timeUs = (int64_t)LiGetMicroseconds() - d->clockOffset;
    if (recording) {
        d->queue.push_back(p);
        d->queueBytes += (size_t)length;
        d->wake.notify_one();
    }
    if (replay && !d->ring.empty()) {
        d->ring.back().packets.push_back(p);
        d->ring.back().bytes += (size_t)length;
        d->ringBytes += (size_t)length;
    }
}

// --- Writing files -----------------------------------------------------------------------------------------------

static QString latestThumb(Capture::Impl* d)
{
    std::lock_guard<std::mutex> g(d->thumbLock);
    return d->thumb;
}

namespace {

// Writes packets into one MP4 (with its sound as AAC). Lives on the thread that writes.
class FileOut
{
public:
    bool open(const Mp4VideoInfo& video, const Mp4AudioInfo* opus, const char* prefix)
    {
        m_Name = newName(prefix, "mp4");
        m_Part = capturesDir() + "/" + m_Name + ".part";
        m_Video = video;
        m_HasAudio = opus != nullptr && m_Track.open(*opus);
        return m_Writer.open(QDir::toNativeSeparators(m_Part).toStdWString(), video, m_HasAudio ? &m_Track.info() : nullptr);
    }
    bool add(const Packet& p)
    {
        if (p.video) {
            return m_Writer.addVideo(p.data->data(), p.data->size(), p.timeUs, p.key);
        }
        if (!m_HasAudio) {
            return true;
        }
        m_Frames.clear();
        m_Track.push(p.data->data(), p.data->size(), p.timeUs, m_Frames);
        for (const AacFrame& f : m_Frames) {
            m_Writer.addAudio(f.data.data(), f.data.size(), f.timeUs);
        }
        return m_Writer.error().empty();
    }
    // Finishes the file and announces it (with a small picture for the panel); false (and nothing announced) if it
    // failed or had no picture.
    bool finish(const char* kind, const QString& thumb)
    {
        if (m_HasAudio) {
            m_Frames.clear();
            m_Track.finish(m_Frames);
            for (const AacFrame& f : m_Frames) {
                m_Writer.addAudio(f.data.data(), f.data.size(), f.timeUs);
            }
        }
        bool started = m_Writer.started();
        int64_t ms = m_Writer.durationMs();
        bool ok = m_Writer.close() && started;
        if (!ok) {
            QFile::remove(m_Part);
            return !started;  // an empty recording (stopped before the first key frame) is not an error
        }
        QJsonObject f;
        f["kind"] = kind;
        f["durationMs"] = (double)ms;
        f["width"] = m_Video.width;
        f["height"] = m_Video.height;
        f["thumb"] = thumb;
        return publish(m_Part, m_Name, f);
    }

private:
    Mp4Writer m_Writer;
    AudioTrack m_Track;
    Mp4VideoInfo m_Video;
    bool m_HasAudio = false;
    QString m_Name, m_Part;
    std::vector<AacFrame> m_Frames;
};

}

void Capture::setRecording(bool on)
{
    Impl* d = impl();
    if (!Bridge::instance().enabled()) {
        return;
    }
    if (!on) {
        if (!m_Recording.exchange(false)) {
            return;
        }
        {
            std::lock_guard<std::mutex> g(d->lock);
            d->stopRecording = true;
            d->wake.notify_one();
        }
        QJsonObject f;
        f["on"] = false;
        Bridge::instance().send("recording", f);
        refreshOverlay(d, false);
        return;
    }
    if (m_Recording.load()) {
        return;
    }
    if (d->recorder.joinable()) {
        d->recorder.join();  // the previous recording is still finishing its file
    }
    Mp4VideoInfo video;
    Mp4AudioInfo opus;
    bool haveAudio;
    {
        std::lock_guard<std::mutex> g(d->lock);
        if (!d->haveVideo) {
            reportError("recording", "no-stream");
            return;
        }
        video = d->video;
        opus = d->opus;
        haveAudio = d->haveAudio;
    }
    QDir().mkpath(capturesDir());
    if (freeBytes(capturesDir()) < kMinFreeToStart) {
        reportError("recording", "disk-full");
        showNotice(d, "disk-full");
        return;
    }
    {
        std::lock_guard<std::mutex> g(d->lock);
        d->queue.clear();
        d->queueBytes = 0;
        d->stopRecording = false;
        d->lastIdrRequestUs = LiGetMicroseconds();
    }
    {
        std::lock_guard<std::mutex> g(d->overlayLock);
        d->recordingSince = SDL_GetTicks();
    }
    m_Recording = true;
    // The file starts at a key frame: one is asked for now instead of waiting for the next.
    LiRequestIdrFrame();
    // And its picture for the panel is the first frame.
    m_ThumbWanted = true;
    Viewport::requestRedraw();

    d->recorder = std::thread([d, video, opus, haveAudio]() {
        auto out = std::make_unique<FileOut>();
        bool ok = out->open(video, haveAudio ? &opus : nullptr, "clip");
        const char* failure = ok ? nullptr : "write";
        Uint32 lastSpaceCheck = SDL_GetTicks();
        for (;;) {
            std::deque<Packet> batch;
            bool stop;
            {
                std::unique_lock<std::mutex> g(d->lock);
                d->wake.wait(g, [d] { return d->stopRecording || !d->queue.empty(); });
                batch.swap(d->queue);
                d->queueBytes = 0;
                stop = d->stopRecording;
            }
            for (const Packet& p : batch) {
                if (failure != nullptr) {
                    break;
                }
                if (p.split) {
                    // New format: this file ends here and the next one starts at the next key frame.
                    if (!out->finish("recording", latestThumb(d))) failure = "write";
                    Mp4VideoInfo next;
                    Mp4AudioInfo nextOpus;
                    bool nextAudio;
                    {
                        std::lock_guard<std::mutex> g(d->lock);
                        next = d->video;
                        nextOpus = d->opus;
                        nextAudio = d->haveAudio;
                    }
                    out = std::make_unique<FileOut>();
                    if (!out->open(next, nextAudio ? &nextOpus : nullptr, "clip")) failure = "write";
                    continue;
                }
                if (!out->add(p)) {
                    failure = "write";
                }
            }
            if (failure == nullptr && SDL_TICKS_PASSED(SDL_GetTicks(), lastSpaceCheck + 5000)) {
                lastSpaceCheck = SDL_GetTicks();
                if (freeBytes(capturesDir()) < kMinFreeWhileRecording) {
                    failure = "disk-full";
                }
            }
            if (stop || failure != nullptr) {
                break;
            }
        }
        bool saved = out->finish("recording", latestThumb(d));
        if (failure == nullptr && !saved) {
            failure = "write";
        }
        if (failure != nullptr) {
            reportError("recording", failure);
            showNotice(d, strcmp(failure, "disk-full") == 0 ? "disk-full" : "error");
            // Stopped on its own: the app and the key both see it as stopped.
            if (Capture::instance().m_Recording.exchange(false)) {
                QJsonObject f;
                f["on"] = false;
                f["reason"] = failure;
                Bridge::instance().send("recording", f);
            }
            std::lock_guard<std::mutex> g(d->lock);
            d->queue.clear();
            d->queueBytes = 0;
        }
        else {
            showNotice(d, "recording");
        }
    });

    QJsonObject f;
    f["on"] = true;
    Bridge::instance().send("recording", f);
    refreshOverlay(d, true);
    ensureOverlayTimer(d);
}

void Capture::configure(bool replay, int seconds, const QString& lang)
{
    Impl* d = impl();
    // Files a past session left behind (the viewer or the app closed while writing or before moving them).
    static std::once_flag swept;
    std::call_once(swept, [] {
        QDir dir(capturesDir());
        const QDateTime old = QDateTime::currentDateTime().addDays(-1);
        for (const QFileInfo& f : dir.entryInfoList(QDir::Files)) {
            if (f.lastModified() < old) {
                QFile::remove(f.absoluteFilePath());
            }
        }
    });
    std::lock_guard<std::mutex> g(d->lock);
    d->lang = lang == QLatin1String("en") ? "en" : "es";
    d->replaySeconds = std::clamp(seconds, 10, 120);
    if (!replay) {
        d->ring.clear();
        d->ringBytes = 0;
        m_ReplayBytes = 0;
    }
    else if (!m_Replay.load()) {
        d->lastIdrRequestUs = 0;
    }
    m_Replay = replay;
}

void Capture::saveReplay()
{
    Impl* d = impl();
    if (!Bridge::instance().enabled()) {
        return;
    }
    if (!m_Replay.load()) {
        reportError("replay", "replay-off");
        showNotice(d, "replay-off");
        return;
    }
    if (d->saving.exchange(true)) {
        return;  // one at a time; the key held down doesn't queue more
    }
    if (d->saver.joinable()) {
        d->saver.join();
    }
    std::vector<Packet> packets;
    Mp4VideoInfo video;
    Mp4AudioInfo opus;
    bool haveAudio;
    {
        std::lock_guard<std::mutex> g(d->lock);
        for (const Segment& s : d->ring) {
            packets.insert(packets.end(), s.packets.begin(), s.packets.end());
        }
        video = d->video;
        opus = d->opus;
        haveAudio = d->haveAudio;
    }
    if (packets.empty()) {
        d->saving = false;
        reportError("replay", "replay-empty");
        showNotice(d, "error");
        return;
    }
    QDir().mkpath(capturesDir());
    unsigned thumbBefore;
    {
        std::lock_guard<std::mutex> g(d->thumbLock);
        thumbBefore = d->thumbSeq;
    }
    m_ThumbWanted = true;
    Viewport::requestRedraw();
    d->saver = std::thread([d, packets = std::move(packets), video, opus, haveAudio, thumbBefore]() {
        FileOut out;
        bool ok = out.open(video, haveAudio ? &opus : nullptr, "replay");
        for (const Packet& p : packets) {
            if (!ok) break;
            ok = out.add(p);
        }
        QString thumb;
        {
            std::unique_lock<std::mutex> g(d->thumbLock);
            d->thumbReady.wait_for(g, std::chrono::milliseconds(1000), [d, thumbBefore] { return d->thumbSeq != thumbBefore; });
            thumb = d->thumb;
        }
        ok = out.finish("replay", thumb) && ok;
        if (!ok) {
            reportError("replay", "write");
        }
        showNotice(d, ok ? "replay" : "error");
        d->saving = false;
    });
}

// --- Screenshot --------------------------------------------------------------------------------------------------

void Capture::screenshot()
{
    Impl* d = impl();
    if (!Bridge::instance().enabled() || m_ShotWanted.exchange(true)) {
        return;
    }
    // The renderer copies the next frame it draws; on a still screen no new frame comes, so the last one is drawn
    // again for it.
    Viewport::requestRedraw();
    d->shotTimer = SDL_AddTimer(1500, [](Uint32, void*) -> Uint32 {
        if (Capture::instance().m_ShotWanted.exchange(false)) {
            reportError("screenshot", "no-frame");
        }
        return 0;
    }, nullptr);
}

void Capture::screenshotFailed(const char* reason)
{
    m_ThumbWanted = false;
    if (m_ShotWanted.exchange(false)) {
        reportError("screenshot", reason);
        showNotice(impl(), "error");
    }
}

void Capture::screenshotTaken(std::unique_ptr<FramePlanes> planes)
{
    Impl* d = impl();
    bool shot = m_ShotWanted.exchange(false);
    bool thumb = m_ThumbWanted.exchange(false);
    if (thumb && !shot) {
        // Only the small picture: every few pixels, scaled down, kept for the next file.
        if (d->thumber.joinable()) {
            d->thumber.join();
        }
        d->thumber = std::thread([d, f = std::move(planes)]() {
            QString t = thumbnailOf(toImage(*f, std::max(1, f->width / 384)));
            std::lock_guard<std::mutex> g(d->thumbLock);
            d->thumb = t;
            d->thumbSeq++;
            d->thumbReady.notify_all();
        });
        return;
    }
    if (!shot) {
        return;
    }
    if (d->shooter.joinable()) {
        d->shooter.join();
    }
    QDir().mkpath(capturesDir());
    d->shooter = std::thread([d, f = std::move(planes), thumb]() {
        QImage img = toImage(*f);
        QString name = newName("shot", "png");
        QString part = capturesDir() + "/" + name + ".part";
        bool ok = !img.isNull() && img.save(part, "PNG");
        QString small = thumbnailOf(img);
        if (thumb) {
            std::lock_guard<std::mutex> g(d->thumbLock);
            d->thumb = small;
            d->thumbSeq++;
            d->thumbReady.notify_all();
        }
        if (ok) {
            QJsonObject fields;
            fields["kind"] = "screenshot";
            fields["width"] = f->width;
            fields["height"] = f->height;
            fields["thumb"] = small;
            ok = publish(part, name, fields);
        }
        else {
            QFile::remove(part);
        }
        if (!ok) {
            reportError("screenshot", "write");
        }
        showNotice(d, ok ? "shot" : "error");
    });
}

// --- End ---------------------------------------------------------------------------------------------------------

void Capture::sessionEnded()
{
    Impl* d = impl();
    setRecording(false);
    if (d->recorder.joinable()) d->recorder.join();
    if (d->saver.joinable()) d->saver.join();
    if (d->shooter.joinable()) d->shooter.join();
    if (d->thumber.joinable()) d->thumber.join();
    m_ThumbWanted = false;
    m_ShotWanted = false;
    m_Replay = false;
    {
        std::lock_guard<std::mutex> g(d->lock);
        d->ring.clear();
        d->ringBytes = 0;
        m_ReplayBytes = 0;
    }
    std::lock_guard<std::mutex> g(d->overlayLock);
    if (d->overlayTimer != 0) {
        SDL_RemoveTimer(d->overlayTimer);
        d->overlayTimer = 0;
    }
    d->notice.clear();
    d->shownText.clear();
}

}
