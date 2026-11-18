#include "mp4writer.h"

#include <algorithm>
#include <cstring>
#include <ctime>

namespace GpuVM {

namespace {

constexpr uint32_t kVideoTimescale = 90000;
constexpr uint32_t kMovieTimescale = 1000;
constexpr uint32_t kVideoTrack = 1;
constexpr uint32_t kAudioTrack = 2;
constexpr size_t kMaxFragmentBytes = 16 * 1024 * 1024;

// Big-endian box builder.
class Buf
{
public:
    std::vector<uint8_t> b;

    void u8(uint32_t v) { b.push_back((uint8_t)v); }
    void u16(uint32_t v) { u8(v >> 8); u8(v); }
    void u24(uint32_t v) { u8(v >> 16); u16(v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v); }
    void u64(uint64_t v) { u32((uint32_t)(v >> 32)); u32((uint32_t)v); }
    void fourcc(const char* c) { b.insert(b.end(), c, c + 4); }
    void bytes(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
    void zeros(size_t n) { b.insert(b.end(), n, 0); }
    size_t begin(const char* type) { size_t at = b.size(); u32(0); fourcc(type); return at; }
    size_t full(const char* type, uint8_t version, uint32_t flags) { size_t at = begin(type); u8(version); u24(flags); return at; }
    void end(size_t at) { put32(at, (uint32_t)(b.size() - at)); }
    void put32(size_t at, uint32_t v) { b[at] = (uint8_t)(v >> 24); b[at + 1] = (uint8_t)(v >> 16); b[at + 2] = (uint8_t)(v >> 8); b[at + 3] = (uint8_t)v; }
};

struct Nal {
    const uint8_t* p;
    size_t n;
};

// The NAL units of an Annex B access unit (start codes of 3 or 4 bytes).
std::vector<Nal> splitAnnexB(const uint8_t* data, size_t size)
{
    std::vector<Nal> out;
    size_t i = 0, start = SIZE_MAX;
    while (i + 2 < size) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            if (start != SIZE_MAX) {
                out.push_back({ data + start, i - start });
            }
            i += 3;
            start = i;
        }
        else {
            i++;
        }
    }
    if (start != SIZE_MAX && start < size) {
        out.push_back({ data + start, size - start });
    }
    // A 4-byte start code leaves its first zero at the end of the previous unit.
    for (Nal& nal : out) {
        while (nal.n > 0 && nal.p[nal.n - 1] == 0) {
            nal.n--;
        }
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](const Nal& n) { return n.n == 0; }), out.end());
    return out;
}

// Payload without the emulation prevention bytes (00 00 03).
std::vector<uint8_t> unescape(const uint8_t* p, size_t n)
{
    std::vector<uint8_t> out;
    out.reserve(n);
    int zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (zeros >= 2 && p[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = p[i] == 0 ? zeros + 1 : 0;
        out.push_back(p[i]);
    }
    return out;
}

class Bits
{
public:
    Bits(const uint8_t* p, size_t n) : m_P(p), m_N(n) {}
    bool ok() const { return m_Ok; }
    uint32_t u(int count)
    {
        uint32_t v = 0;
        for (int i = 0; i < count; i++) {
            if (m_Pos >= m_N * 8) {
                m_Ok = false;
                return 0;
            }
            v = (v << 1) | ((m_P[m_Pos >> 3] >> (7 - (m_Pos & 7))) & 1);
            m_Pos++;
        }
        return v;
    }
    uint32_t ue()
    {
        int zeros = 0;
        while (u(1) == 0 && m_Ok) {
            if (++zeros > 31) {
                m_Ok = false;
                return 0;
            }
        }
        return ((1u << zeros) - 1) + u(zeros);
    }
    uint32_t uvlc()
    {
        int zeros = 0;
        while (u(1) == 0 && m_Ok) {
            if (++zeros >= 32) {
                m_Ok = false;
                return 0;
            }
        }
        return zeros ? ((1u << zeros) - 1) + u(zeros) : 0;
    }
    void skip(int count) { u(0); m_Pos += count; if (m_Pos > m_N * 8) m_Ok = false; }

private:
    const uint8_t* m_P;
    size_t m_N;
    size_t m_Pos = 0;
    bool m_Ok = true;
};

uint64_t readLeb128(const uint8_t* p, size_t n, size_t* used)
{
    uint64_t v = 0;
    for (size_t i = 0; i < 8 && i < n; i++) {
        v |= (uint64_t)(p[i] & 0x7f) << (7 * i);
        if (!(p[i] & 0x80)) {
            *used = i + 1;
            return v;
        }
    }
    *used = 0;
    return 0;
}

void writeLeb128(std::vector<uint8_t>& out, uint64_t v)
{
    do {
        uint8_t byte = v & 0x7f;
        v >>= 7;
        out.push_back(byte | (v ? 0x80 : 0));
    } while (v);
}

struct Obu {
    int type;
    const uint8_t* header;  // header (and extension) bytes
    size_t headerSize;
    const uint8_t* payload;
    size_t payloadSize;
};

// The OBUs of an AV1 temporal unit (low overhead format; the last one may omit its size).
bool splitObus(const uint8_t* data, size_t size, std::vector<Obu>& out)
{
    size_t i = 0;
    while (i < size) {
        uint8_t h = data[i];
        if (h & 0x80) {
            return false;  // forbidden bit
        }
        bool ext = (h >> 2) & 1, hasSize = (h >> 1) & 1;
        size_t hs = ext ? 2 : 1;
        if (i + hs > size) {
            return false;
        }
        size_t payloadSize, used = 0;
        if (hasSize) {
            payloadSize = (size_t)readLeb128(data + i + hs, size - i - hs, &used);
            if (used == 0 || i + hs + used + payloadSize > size) {
                return false;
            }
        }
        else {
            payloadSize = size - i - hs;
        }
        out.push_back({ (h >> 3) & 0xf, data + i, hs, data + i + hs + used, payloadSize });
        i += hs + used + payloadSize;
    }
    return true;
}

struct HevcInfo {
    uint8_t ptl[12] = {};
    int chroma = 1;
    int depthLuma = 0;
    int depthChroma = 0;
    int subLayers = 1;
    bool nested = true;
};

bool parseHevcSps(const uint8_t* nal, size_t n, HevcInfo& info)
{
    std::vector<uint8_t> rbsp = unescape(nal + 2, n > 2 ? n - 2 : 0);
    Bits b(rbsp.data(), rbsp.size());
    b.u(4);  // vps id
    int maxSub = b.u(3);
    info.subLayers = maxSub + 1;
    info.nested = b.u(1) != 0;
    for (int i = 0; i < 12; i++) {
        info.ptl[i] = (uint8_t)b.u(8);
    }
    bool profilePresent[8] = {}, levelPresent[8] = {};
    for (int i = 0; i < maxSub; i++) {
        profilePresent[i] = b.u(1);
        levelPresent[i] = b.u(1);
    }
    if (maxSub > 0) {
        for (int i = maxSub; i < 8; i++) {
            b.u(2);
        }
    }
    for (int i = 0; i < maxSub; i++) {
        if (profilePresent[i]) b.skip(88);
        if (levelPresent[i]) b.skip(8);
    }
    b.ue();  // sps id
    info.chroma = b.ue();
    if (info.chroma == 3) b.u(1);
    b.ue();  // width
    b.ue();  // height
    if (b.u(1)) {
        b.ue(); b.ue(); b.ue(); b.ue();
    }
    info.depthLuma = b.ue();
    info.depthChroma = b.ue();
    return b.ok();
}

struct Av1Info {
    int profile = 0;
    int level = 0;
    int tier = 0;
    int highBitdepth = 0;
    int twelveBit = 0;
    int mono = 0;
    int ssx = 1;
    int ssy = 1;
    int chromaPosition = 0;
};

bool parseAv1SequenceHeader(const uint8_t* p, size_t n, Av1Info& info)
{
    Bits b(p, n);
    info.profile = b.u(3);
    b.u(1);  // still picture
    bool reduced = b.u(1);
    bool decoderModel = false;
    int bufferDelayLength = 0;
    if (reduced) {
        info.level = b.u(5);
    }
    else {
        if (b.u(1)) {  // timing info
            b.u(32); b.u(32);
            if (b.u(1)) b.uvlc();
            decoderModel = b.u(1);
            if (decoderModel) {
                bufferDelayLength = b.u(5) + 1;
                b.u(32); b.u(5); b.u(5);
            }
        }
        bool initialDisplayDelay = b.u(1);
        int points = b.u(5) + 1;
        for (int i = 0; i < points; i++) {
            b.u(12);
            int level = b.u(5);
            int tier = level > 7 ? b.u(1) : 0;
            if (i == 0) {
                info.level = level;
                info.tier = tier;
            }
            if (decoderModel && b.u(1)) {
                b.u(bufferDelayLength); b.u(bufferDelayLength); b.u(1);
            }
            if (initialDisplayDelay && b.u(1)) {
                b.u(4);
            }
        }
    }
    int wBits = b.u(4) + 1, hBits = b.u(4) + 1;
    b.u(wBits); b.u(hBits);
    if (!reduced && b.u(1)) {  // frame ids
        b.u(4); b.u(3);
    }
    b.u(1); b.u(1); b.u(1);  // 128x128, filter intra, intra edge
    if (!reduced) {
        b.u(1); b.u(1); b.u(1); b.u(1);  // interintra, masked, warped, dual filter
        bool orderHint = b.u(1);
        if (orderHint) { b.u(1); b.u(1); }
        int screenTools = b.u(1) ? 2 : (int)b.u(1);
        if (screenTools > 0 && !b.u(1)) b.u(1);
        if (orderHint) b.u(3);
    }
    b.u(1); b.u(1); b.u(1);  // superres, cdef, restoration
    info.highBitdepth = b.u(1);
    int depth = 8;
    if (info.profile == 2 && info.highBitdepth) {
        info.twelveBit = b.u(1);
        depth = info.twelveBit ? 12 : 10;
    }
    else if (info.highBitdepth) {
        depth = 10;
    }
    info.mono = info.profile == 1 ? 0 : b.u(1);
    int primaries = 2, transfer = 2, matrix = 2;
    if (b.u(1)) {
        primaries = b.u(8); transfer = b.u(8); matrix = b.u(8);
    }
    if (info.mono) {
        info.ssx = info.ssy = 1;
    }
    else if (primaries == 1 && transfer == 13 && matrix == 0) {
        info.ssx = info.ssy = 0;
    }
    else {
        b.u(1);  // color range
        if (info.profile == 0) { info.ssx = info.ssy = 1; }
        else if (info.profile == 1) { info.ssx = info.ssy = 0; }
        else if (depth == 12) {
            info.ssx = b.u(1);
            info.ssy = info.ssx ? b.u(1) : 0;
        }
        else { info.ssx = 1; info.ssy = 0; }
        if (info.ssx && info.ssy) info.chromaPosition = b.u(2);
    }
    return b.ok();
}

uint32_t creationTime()
{
    return (uint32_t)((uint64_t)time(nullptr) + 2082844800ull);  // seconds since 1904
}

void matrix(Buf& o)
{
    const uint32_t m[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000 };
    for (uint32_t v : m) o.u32(v);
}

void dinf(Buf& o)
{
    size_t d = o.begin("dinf");
    size_t r = o.full("dref", 0, 0);
    o.u32(1);
    size_t u = o.full("url ", 0, 1);
    o.end(u);
    o.end(r);
    o.end(d);
}

void emptyTables(Buf& o)
{
    size_t a = o.full("stts", 0, 0); o.u32(0); o.end(a);
    a = o.full("stsc", 0, 0); o.u32(0); o.end(a);
    a = o.full("stsz", 0, 0); o.u32(0); o.u32(0); o.end(a);
    a = o.full("stco", 0, 0); o.u32(0); o.end(a);
}

}

Mp4Writer::~Mp4Writer()
{
    if (m_File != nullptr) {
        fclose(m_File);
    }
}

bool Mp4Writer::fail(const char* what)
{
    if (!m_Failed) {
        m_Error = what;
    }
    m_Failed = true;
    return false;
}

bool Mp4Writer::writeBytes(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty()) {
        return true;
    }
    if (fwrite(bytes.data(), 1, bytes.size(), m_File) != bytes.size()) {
        return fail("write");
    }
    m_Offset += bytes.size();
    return true;
}

bool Mp4Writer::open(const std::wstring& path, const Mp4VideoInfo& video, const Mp4AudioInfo* audio)
{
    m_Video = video;
    m_HasAudio = audio != nullptr && audio->channels > 0 && audio->channels <= 8 && audio->samplesPerPacket > 0;
    if (m_HasAudio) {
        m_Audio = *audio;
    }
#ifdef _WIN32
    // Exclusive: nobody else reads or writes it until it is finished.
    if (_wfopen_s(&m_File, path.c_str(), L"wbx") != 0) {
        m_File = nullptr;
    }
#else
    m_File = fopen(std::string(path.begin(), path.end()).c_str(), "wbx");
#endif
    if (m_File == nullptr) {
        return fail("create");
    }
    setvbuf(m_File, nullptr, _IOFBF, 1 << 20);
    return true;
}

bool Mp4Writer::toSample(const uint8_t* data, size_t size, std::vector<uint8_t>& out)
{
    if (m_Video.codec == VideoCodec::AV1) {
        std::vector<Obu> obus;
        if (!splitObus(data, size, obus)) {
            return false;
        }
        for (const Obu& o : obus) {
            if (o.type == 2 || o.type == 15) {
                continue;  // temporal delimiters and padding are not stored
            }
            // Every OBU in a sample carries its size.
            out.push_back(o.header[0] | 0x02);
            out.insert(out.end(), o.header + 1, o.header + o.headerSize);
            writeLeb128(out, o.payloadSize);
            out.insert(out.end(), o.payload, o.payload + o.payloadSize);
        }
        return true;
    }
    for (const Nal& nal : splitAnnexB(data, size)) {
        int type = m_Video.codec == VideoCodec::H264 ? (nal.p[0] & 0x1f) : ((nal.p[0] >> 1) & 0x3f);
        bool skip = m_Video.codec == VideoCodec::H264
                        ? (type == 7 || type == 8 || type == 9 || type == 12)
                        : (type == 32 || type == 33 || type == 34 || type == 35 || type == 38);
        if (skip) {
            continue;  // parameter sets live in the sample entry; delimiters and filler are dropped
        }
        uint32_t n = (uint32_t)nal.n;
        out.push_back((uint8_t)(n >> 24));
        out.push_back((uint8_t)(n >> 16));
        out.push_back((uint8_t)(n >> 8));
        out.push_back((uint8_t)n);
        out.insert(out.end(), nal.p, nal.p + nal.n);
    }
    return true;
}

bool Mp4Writer::writeHeader(const uint8_t* key, size_t size)
{
    // The codec's configuration, from the parameter sets the first key frame carries.
    Buf config;
    const char* entryType = "avc1";
    if (m_Video.codec == VideoCodec::H264) {
        const Nal* sps = nullptr;
        const Nal* pps = nullptr;
        std::vector<Nal> nals = splitAnnexB(key, size);
        for (const Nal& nal : nals) {
            int type = nal.p[0] & 0x1f;
            if (type == 7 && !sps) sps = &nal;
            if (type == 8 && !pps) pps = &nal;
        }
        if (!sps || !pps || sps->n < 4) {
            return fail("config");
        }
        size_t c = config.begin("avcC");
        config.u8(1);
        config.u8(sps->p[1]);
        config.u8(sps->p[2]);
        config.u8(sps->p[3]);
        config.u8(0xff);  // 4-byte lengths
        config.u8(0xe1);
        config.u16((uint32_t)sps->n);
        config.bytes(sps->p, sps->n);
        config.u8(1);
        config.u16((uint32_t)pps->n);
        config.bytes(pps->p, pps->n);
        int profile = sps->p[1];
        if (profile == 100 || profile == 110 || profile == 122 || profile == 244) {
            std::vector<uint8_t> rbsp = unescape(sps->p + 4, sps->n - 4);
            Bits b(rbsp.data(), rbsp.size());
            b.ue();
            int chroma = b.ue();
            if (chroma == 3) b.u(1);
            int depthLuma = b.ue(), depthChroma = b.ue();
            config.u8(0xfc | (chroma & 3));
            config.u8(0xf8 | (depthLuma & 7));
            config.u8(0xf8 | (depthChroma & 7));
            config.u8(0);
        }
        config.end(c);
        entryType = "avc1";
    }
    else if (m_Video.codec == VideoCodec::HEVC) {
        const Nal* sets[3] = {};
        std::vector<Nal> nals = splitAnnexB(key, size);
        for (const Nal& nal : nals) {
            int type = (nal.p[0] >> 1) & 0x3f;
            if (type >= 32 && type <= 34 && !sets[type - 32]) sets[type - 32] = &nal;
        }
        HevcInfo info;
        if (!sets[0] || !sets[1] || !sets[2] || !parseHevcSps(sets[1]->p, sets[1]->n, info)) {
            return fail("config");
        }
        size_t c = config.begin("hvcC");
        config.u8(1);
        config.bytes(info.ptl, 12);
        config.u16(0xf000);
        config.u8(0xfc);
        config.u8(0xfc | (info.chroma & 3));
        config.u8(0xf8 | (info.depthLuma & 7));
        config.u8(0xf8 | (info.depthChroma & 7));
        config.u16(0);
        config.u8(((info.subLayers & 7) << 3) | ((info.nested ? 1 : 0) << 2) | 3);
        config.u8(3);
        for (int i = 0; i < 3; i++) {
            config.u8(0x80 | (32 + i));
            config.u16(1);
            config.u16((uint32_t)sets[i]->n);
            config.bytes(sets[i]->p, sets[i]->n);
        }
        config.end(c);
        entryType = "hvc1";
    }
    else {
        std::vector<Obu> obus;
        const Obu* seq = nullptr;
        if (splitObus(key, size, obus)) {
            for (const Obu& o : obus) {
                if (o.type == 1) { seq = &o; break; }
            }
        }
        Av1Info info;
        if (!seq || !parseAv1SequenceHeader(seq->payload, seq->payloadSize, info)) {
            return fail("config");
        }
        size_t c = config.begin("av1C");
        config.u8(0x81);
        config.u8(((info.profile & 7) << 5) | (info.level & 0x1f));
        config.u8((info.tier << 7) | (info.highBitdepth << 6) | (info.twelveBit << 5) | (info.mono << 4) |
                  (info.ssx << 3) | (info.ssy << 2) | (info.chromaPosition & 3));
        config.u8(0);
        config.u8(seq->header[0] | 0x02);
        config.bytes(seq->header + 1, seq->headerSize - 1);
        std::vector<uint8_t> leb;
        writeLeb128(leb, seq->payloadSize);
        config.bytes(leb.data(), leb.size());
        config.bytes(seq->payload, seq->payloadSize);
        config.end(c);
        entryType = "av01";
    }

    Buf o;
    size_t box = o.begin("ftyp");
    o.fourcc("isom");
    o.u32(0x200);
    o.fourcc("isom");
    o.fourcc("iso6");
    o.fourcc("mp41");
    if (m_Video.codec == VideoCodec::AV1) o.fourcc("av01");
    o.end(box);

    uint32_t now = creationTime();
    size_t moov = o.begin("moov");
    box = o.full("mvhd", 0, 0);
    o.u32(now); o.u32(now);
    o.u32(kMovieTimescale);
    m_MvhdDurationOffset = o.b.size();
    o.u32(0);
    o.u32(0x00010000); o.u16(0x0100); o.zeros(10);
    matrix(o);
    o.zeros(24);
    o.u32(m_HasAudio ? 3 : 2);
    o.end(box);

    // Video track.
    size_t trak = o.begin("trak");
    box = o.full("tkhd", 0, 3);
    o.u32(now); o.u32(now); o.u32(kVideoTrack); o.u32(0); o.u32(0); o.zeros(8);
    o.u16(0); o.u16(0); o.u16(0); o.u16(0);
    matrix(o);
    o.u32((uint32_t)m_Video.width << 16); o.u32((uint32_t)m_Video.height << 16);
    o.end(box);
    size_t mdia = o.begin("mdia");
    box = o.full("mdhd", 0, 0);
    o.u32(now); o.u32(now); o.u32(kVideoTimescale); o.u32(0); o.u16(0x55c4); o.u16(0);
    o.end(box);
    box = o.full("hdlr", 0, 0);
    o.u32(0); o.fourcc("vide"); o.zeros(12);
    o.bytes((const uint8_t*)"GpuVM video", 12);
    o.end(box);
    size_t minf = o.begin("minf");
    box = o.full("vmhd", 0, 1); o.zeros(8); o.end(box);
    dinf(o);
    size_t stbl = o.begin("stbl");
    size_t stsd = o.full("stsd", 0, 0);
    o.u32(1);
    size_t entry = o.begin(entryType);
    o.zeros(6); o.u16(1);
    o.u16(0); o.u16(0); o.zeros(12);
    o.u16((uint32_t)m_Video.width); o.u16((uint32_t)m_Video.height);
    o.u32(0x00480000); o.u32(0x00480000); o.u32(0); o.u16(1);
    o.zeros(32);
    o.u16(0x18); o.u16(0xffff);
    o.bytes(config.b.data(), config.b.size());
    box = o.begin("colr");
    o.fourcc("nclx");
    o.u16((uint32_t)m_Video.primaries); o.u16((uint32_t)m_Video.transfer); o.u16((uint32_t)m_Video.matrix);
    o.u8(m_Video.fullRange ? 0x80 : 0);
    o.end(box);
    o.end(entry);
    o.end(stsd);
    emptyTables(o);
    o.end(stbl);
    o.end(minf);
    o.end(mdia);
    o.end(trak);

    if (m_HasAudio) {
        trak = o.begin("trak");
        box = o.full("tkhd", 0, 3);
        o.u32(now); o.u32(now); o.u32(kAudioTrack); o.u32(0); o.u32(0); o.zeros(8);
        o.u16(0); o.u16(1); o.u16(0x0100); o.u16(0);
        matrix(o);
        o.u32(0); o.u32(0);
        o.end(box);
        mdia = o.begin("mdia");
        box = o.full("mdhd", 0, 0);
        o.u32(now); o.u32(now); o.u32((uint32_t)m_Audio.sampleRate); o.u32(0); o.u16(0x55c4); o.u16(0);
        o.end(box);
        box = o.full("hdlr", 0, 0);
        o.u32(0); o.fourcc("soun"); o.zeros(12);
        o.bytes((const uint8_t*)"GpuVM audio", 12);
        o.end(box);
        minf = o.begin("minf");
        box = o.full("smhd", 0, 0); o.u16(0); o.u16(0); o.end(box);
        dinf(o);
        stbl = o.begin("stbl");
        stsd = o.full("stsd", 0, 0);
        o.u32(1);
        entry = o.begin(m_Audio.aac ? "mp4a" : "Opus");
        o.zeros(6); o.u16(1);
        o.zeros(8);
        o.u16((uint32_t)m_Audio.channels); o.u16(16); o.u16(0); o.u16(0);
        o.u32((uint32_t)m_Audio.sampleRate << 16);
        if (m_Audio.aac) {
            // ES descriptor: MPEG-4 audio (0x40), audio stream, the encoder's AudioSpecificConfig.
            const std::vector<uint8_t>& asc = m_Audio.aacConfig;
            box = o.full("esds", 0, 0);
            o.u8(0x03); o.u8((uint32_t)(23 + asc.size()));
            o.u16(0); o.u8(0);
            o.u8(0x04); o.u8((uint32_t)(15 + asc.size()));
            o.u8(0x40); o.u8(0x15); o.u24(0);
            o.u32((uint32_t)m_Audio.bitrate); o.u32((uint32_t)m_Audio.bitrate);
            o.u8(0x05); o.u8((uint32_t)asc.size());
            o.bytes(asc.data(), asc.size());
            o.u8(0x06); o.u8(1); o.u8(0x02);
            o.end(box);
            o.end(entry);
        }
        else {
        box = o.begin("dOps");
        bool family1 = m_Audio.channels > 2;
        o.u8(0);
        o.u8((uint32_t)m_Audio.channels);
        o.u16(0);  // pre-skip: the recording starts mid-stream, nothing to trim
        o.u32((uint32_t)m_Audio.sampleRate);
        o.u16(0);
        o.u8(family1 ? 1 : 0);
        if (family1) {
            o.u8((uint32_t)m_Audio.streams);
            o.u8((uint32_t)m_Audio.coupledStreams);
            for (int i = 0; i < m_Audio.channels; i++) o.u8(m_Audio.mapping[i]);
        }
        o.end(box);
        o.end(entry);
        }
        o.end(stsd);
        emptyTables(o);
        o.end(stbl);
        o.end(minf);
        o.end(mdia);
        o.end(trak);
    }

    size_t mvex = o.begin("mvex");
    box = o.full("mehd", 1, 0);
    m_MehdOffset = o.b.size();
    o.u64(0);
    o.end(box);
    box = o.full("trex", 0, 0);
    o.u32(kVideoTrack); o.u32(1); o.u32(0); o.u32(0); o.u32(0x01010000);
    o.end(box);
    if (m_HasAudio) {
        box = o.full("trex", 0, 0);
        o.u32(kAudioTrack); o.u32(1); o.u32(0); o.u32(0); o.u32(0x02000000);
        o.end(box);
    }
    o.end(mvex);
    o.end(moov);

    m_MvhdDurationOffset += m_Offset;
    m_MehdOffset += m_Offset;
    return writeBytes(o.b);
}

bool Mp4Writer::addVideo(const uint8_t* data, size_t size, int64_t timeUs, bool key)
{
    if (m_File == nullptr || m_Failed) {
        return false;
    }
    if (!m_Started) {
        if (!key) {
            return true;
        }
        if (!writeHeader(data, size)) {
            return false;
        }
        m_Started = true;
        m_StartUs = timeUs;
    }
    int64_t rel = std::max<int64_t>(0, timeUs - m_StartUs);
    uint64_t t = (uint64_t)rel * kVideoTimescale / 1000000;
    if (m_HaveVideo) {
        if (t <= m_LastVideoTime) {
            t = m_LastVideoTime + 1;
        }
        uint64_t d = std::min<uint64_t>(t - m_LastVideoTime, 60ull * kVideoTimescale);
        m_VideoSamples.back().duration = (uint32_t)d;
        m_LastVideoDuration = (uint32_t)d;
        m_VideoEnd = m_LastVideoTime + d;
        if (key || m_VideoEnd - m_VideoFragmentStart >= kVideoTimescale || m_VideoData.size() >= kMaxFragmentBytes) {
            if (!flushFragment()) {
                return false;
            }
        }
    }
    if (m_VideoSamples.empty()) {
        m_VideoFragmentStart = t;
    }
    size_t before = m_VideoData.size();
    if (!toSample(data, size, m_VideoData)) {
        m_VideoData.resize(before);
        // A frame that can't be read is dropped; the next ones still decode as well as they did live.
        m_HaveVideo = !m_VideoSamples.empty();
        return true;
    }
    m_VideoSamples.push_back({ (uint32_t)(m_VideoData.size() - before), 0, key });
    m_LastVideoTime = t;
    m_HaveVideo = true;
    return true;
}

bool Mp4Writer::addAudio(const uint8_t* data, size_t size, int64_t timeUs)
{
    if (!m_HasAudio || !m_Started || m_Failed || size == 0) {
        return !m_Failed;
    }
    int64_t rel = timeUs - m_StartUs;
    if (rel < 0) {
        return true;
    }
    uint64_t rate = (uint64_t)m_Audio.sampleRate;
    uint64_t expected = (uint64_t)rel * rate / 1000000;
    uint64_t slack = rate / 5;  // 200 ms
    if (!m_AudioAnchored) {
        m_AudioNext = expected;
        m_AudioFragmentStart = expected;
        m_AudioAnchored = true;
    }
    else if (expected > m_AudioNext + slack) {
        // Audio stopped for a while: the gap stays silent instead of pulling the rest ahead of the picture.
        if (!m_AudioSamples.empty()) {
            m_AudioSamples.back().duration += (uint32_t)(expected - m_AudioNext);
        }
        else {
            m_AudioFragmentStart = expected;
        }
        m_AudioNext = expected;
    }
    else if (expected + slack < m_AudioNext) {
        return true;  // far ahead of its time (a burst after a stall): dropped
    }
    if (m_AudioSamples.empty()) {
        m_AudioFragmentStart = m_AudioNext;
    }
    m_AudioData.insert(m_AudioData.end(), data, data + size);
    m_AudioSamples.push_back({ (uint32_t)size, (uint32_t)m_Audio.samplesPerPacket, true });
    m_AudioNext += (uint64_t)m_Audio.samplesPerPacket;
    // Nothing new on the screen means no new frames: the sound is written anyway, about every second.
    if (m_AudioNext - m_AudioFragmentStart >= rate * 2) {
        return flushFragment();
    }
    return true;
}

bool Mp4Writer::flushFragment()
{
    // The newest frame's duration is only known when the next one comes: it waits for the next fragment.
    size_t nv = m_VideoSamples.size();
    if (nv > 0 && m_VideoSamples.back().duration == 0) {
        nv--;
    }
    size_t na = m_AudioSamples.size();
    if (nv == 0 && na == 0) {
        return true;
    }
    size_t videoBytes = 0;
    for (size_t i = 0; i < nv; i++) videoBytes += m_VideoSamples[i].size;
    size_t audioBytes = m_AudioData.size();

    Buf o;
    size_t moof = o.begin("moof");
    size_t box = o.full("mfhd", 0, 0);
    o.u32(++m_Sequence);
    o.end(box);
    size_t videoOffsetAt = 0, audioOffsetAt = 0;
    if (nv > 0) {
        size_t traf = o.begin("traf");
        box = o.full("tfhd", 0, 0x020000);
        o.u32(kVideoTrack);
        o.end(box);
        box = o.full("tfdt", 1, 0);
        o.u64(m_VideoFragmentStart);
        o.end(box);
        box = o.full("trun", 0, 0x000701);
        o.u32((uint32_t)nv);
        videoOffsetAt = o.b.size();
        o.u32(0);
        for (size_t i = 0; i < nv; i++) {
            const Sample& s = m_VideoSamples[i];
            o.u32(s.duration);
            o.u32(s.size);
            o.u32(s.key ? 0x02000000 : 0x01010000);
        }
        o.end(box);
        o.end(traf);
    }
    if (na > 0) {
        size_t traf = o.begin("traf");
        box = o.full("tfhd", 0, 0x020000);
        o.u32(kAudioTrack);
        o.end(box);
        box = o.full("tfdt", 1, 0);
        o.u64(m_AudioFragmentStart);
        o.end(box);
        box = o.full("trun", 0, 0x000301);
        o.u32((uint32_t)na);
        audioOffsetAt = o.b.size();
        o.u32(0);
        for (const Sample& s : m_AudioSamples) {
            o.u32(s.duration);
            o.u32(s.size);
        }
        o.end(box);
        o.end(traf);
    }
    o.end(moof);
    uint32_t moofSize = (uint32_t)o.b.size();
    if (nv > 0) o.put32(videoOffsetAt, moofSize + 8);
    if (na > 0) o.put32(audioOffsetAt, moofSize + 8 + (uint32_t)videoBytes);
    o.u32((uint32_t)(8 + videoBytes + audioBytes));
    o.fourcc("mdat");

    if (nv > 0 && m_VideoSamples[0].key) {
        m_Keys.push_back({ m_VideoFragmentStart, m_Offset });
    }
    if (!writeBytes(o.b)) {
        return false;
    }
    if (videoBytes > 0 && fwrite(m_VideoData.data(), 1, videoBytes, m_File) != videoBytes) {
        return fail("write");
    }
    m_Offset += videoBytes;
    if (audioBytes > 0 && fwrite(m_AudioData.data(), 1, audioBytes, m_File) != audioBytes) {
        return fail("write");
    }
    m_Offset += audioBytes;

    uint64_t written = 0;
    for (size_t i = 0; i < nv; i++) written += m_VideoSamples[i].duration;
    m_VideoFragmentStart += written;
    m_VideoData.erase(m_VideoData.begin(), m_VideoData.begin() + videoBytes);
    m_VideoSamples.erase(m_VideoSamples.begin(), m_VideoSamples.begin() + nv);
    m_AudioData.clear();
    m_AudioSamples.clear();
    m_AudioFragmentStart = m_AudioNext;
    return true;
}

int64_t Mp4Writer::durationMs() const
{
    int64_t video = (int64_t)(m_VideoEnd * 1000 / kVideoTimescale);
    int64_t audio = m_HasAudio && m_AudioAnchored ? (int64_t)(m_AudioNext * 1000 / (uint64_t)m_Audio.sampleRate) : 0;
    return std::max(video, audio);
}

bool Mp4Writer::close()
{
    if (m_File == nullptr) {
        return false;
    }
    bool ok = !m_Failed;
    if (ok && m_Started) {
        if (!m_VideoSamples.empty() && m_VideoSamples.back().duration == 0) {
            m_VideoSamples.back().duration = m_LastVideoDuration;
            m_VideoEnd = m_LastVideoTime + m_LastVideoDuration;
        }
        ok = flushFragment();
        if (ok) {
            // The index of the key frames, for seeking.
            Buf o;
            size_t mfra = o.begin("mfra");
            size_t box = o.full("tfra", 1, 0);
            o.u32(kVideoTrack);
            o.u32(0);
            o.u32((uint32_t)m_Keys.size());
            for (const KeyEntry& k : m_Keys) {
                o.u64(k.time);
                o.u64(k.moofOffset);
                o.u8(1); o.u8(1); o.u8(1);
            }
            o.end(box);
            box = o.full("mfro", 0, 0);
            o.u32((uint32_t)(o.b.size() - mfra + 4));
            o.end(box);
            o.end(mfra);
            ok = writeBytes(o.b);
        }
        if (ok) {
            // The duration, now that it is known.
            uint64_t ms = (uint64_t)durationMs();
            uint8_t d64[8], d32[4];
            for (int i = 0; i < 8; i++) d64[i] = (uint8_t)(ms >> (56 - 8 * i));
            for (int i = 0; i < 4; i++) d32[i] = (uint8_t)(ms >> (24 - 8 * i));
            ok = fflush(m_File) == 0 &&
                 _fseeki64(m_File, (int64_t)m_MehdOffset, SEEK_SET) == 0 && fwrite(d64, 1, 8, m_File) == 8 &&
                 _fseeki64(m_File, (int64_t)m_MvhdDurationOffset, SEEK_SET) == 0 && fwrite(d32, 1, 4, m_File) == 4;
            if (!ok) fail("write");
        }
    }
    if (fclose(m_File) != 0 && ok) {
        ok = fail("write");
    }
    m_File = nullptr;
    return ok;
}

}
