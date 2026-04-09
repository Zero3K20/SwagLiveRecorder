#include "WebMMuxer.h"

#include <cstring>
#include <cassert>

// ─── EBML element IDs ─────────────────────────────────────────────────────────
// Written big-endian as-is; their width is encoded in the leading bits.

enum : uint32_t {
    ID_EBML                = 0x1A45DFA3,
    ID_DocType             = 0x4282,
    ID_DocTypeVersion      = 0x4287,
    ID_DocTypeReadVersion  = 0x4285,

    ID_Segment             = 0x18538067,

    ID_Info                = 0x1549A966,
    ID_TimecodeScale       = 0x2AD7B1,
    ID_MuxingApp           = 0x4D80,
    ID_WritingApp          = 0x5741,

    ID_Tracks              = 0x1654AE6B,
    ID_TrackEntry          = 0xAE,
    ID_TrackNumber         = 0xD7,
    ID_TrackUID            = 0x73C5,
    ID_TrackType           = 0x83,
    ID_CodecID             = 0x86,
    ID_CodecPrivate        = 0x63A2,
    ID_CodecDelay          = 0x56AA,
    ID_SeekPreRoll         = 0x56BB,

    ID_Video               = 0xE0,
    ID_PixelWidth          = 0xB0,
    ID_PixelHeight         = 0xBA,

    ID_Audio               = 0xE1,
    ID_SamplingFreq        = 0xB5,
    ID_Channels            = 0x9F,

    ID_Cluster             = 0x1F43B675,
    ID_Timecode            = 0xE7,
    ID_SimpleBlock         = 0xA3,
};

// ─── EBML builder helpers ─────────────────────────────────────────────────────

// Byte-width of an EBML element ID (determined by leading-bit marker).
static int id_width(uint32_t id) {
    if (id & 0xFF000000) return 4;
    if (id & 0x00FF0000) return 3;
    if (id & 0x0000FF00) return 2;
    return 1;
}

// Append an EBML element ID (big-endian).
static void put_id(std::vector<uint8_t>& v, uint32_t id) {
    int w = id_width(id);
    for (int i = w - 1; i >= 0; --i)
        v.push_back(static_cast<uint8_t>((id >> (8 * i)) & 0xFF));
}

// Append an EBML VINT-encoded element size.
// Ranges: 1B [0..126], 2B [0..16382], 3B [0..2097150], 4B [0..268435454].
static void put_size(std::vector<uint8_t>& v, uint64_t sz) {
    if (sz < 0x7F) {
        v.push_back(static_cast<uint8_t>(0x80 | sz));
    } else if (sz < 0x3FFF) {
        v.push_back(static_cast<uint8_t>(0x40 | (sz >> 8)));
        v.push_back(static_cast<uint8_t>(sz & 0xFF));
    } else if (sz < 0x1FFFFF) {
        v.push_back(static_cast<uint8_t>(0x20 | (sz >> 16)));
        v.push_back(static_cast<uint8_t>((sz >>  8) & 0xFF));
        v.push_back(static_cast<uint8_t>(sz & 0xFF));
    } else if (sz < 0x0FFFFFFF) {
        v.push_back(static_cast<uint8_t>(0x10 | (sz >> 24)));
        v.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
        v.push_back(static_cast<uint8_t>((sz >>  8) & 0xFF));
        v.push_back(static_cast<uint8_t>(sz & 0xFF));
    } else {
        // 5-byte VINT for sizes up to ~34 GB (ample for any single element we build)
        v.push_back(static_cast<uint8_t>(0x08 | ((sz >> 32) & 0x07)));
        v.push_back(static_cast<uint8_t>((sz >> 24) & 0xFF));
        v.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
        v.push_back(static_cast<uint8_t>((sz >>  8) & 0xFF));
        v.push_back(static_cast<uint8_t>(sz & 0xFF));
    }
}

// Append a big-endian unsigned integer of exactly `bytes` bytes.
static void put_uint(std::vector<uint8_t>& v, uint64_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i)
        v.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
}

// Append a big-endian IEEE 754 double.
static void put_float64(std::vector<uint8_t>& v, double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, 8);
    put_uint(v, bits, 8);
}

// Concatenate src onto dst.
static void app(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

// Build a complete EBML element: ID + VINT(size) + data.
static std::vector<uint8_t> elem(uint32_t id, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> r;
    r.reserve(4 + 8 + data.size());
    put_id(r, id);
    put_size(r, data.size());
    app(r, data);
    return r;
}

// Build a uint element using the minimum byte count needed for the value.
static std::vector<uint8_t> elem_uint(uint32_t id, uint64_t value) {
    int bytes = 1;
    uint64_t tmp = value >> 8;
    while (tmp) { ++bytes; tmp >>= 8; }
    std::vector<uint8_t> data;
    put_uint(data, value, bytes);
    return elem(id, data);
}

// Build a float64 element (8-byte IEEE 754 big-endian).
static std::vector<uint8_t> elem_f64(uint32_t id, double value) {
    std::vector<uint8_t> data;
    put_float64(data, value);
    return elem(id, data);
}

// Build a string element (UTF-8, no null terminator).
static std::vector<uint8_t> elem_str(uint32_t id, const std::string& s) {
    return elem(id, std::vector<uint8_t>(s.begin(), s.end()));
}

// ─── WebMMuxer ────────────────────────────────────────────────────────────────

WebMMuxer::WebMMuxer(WriteCallback cb) : m_write(std::move(cb)) {}

void WebMMuxer::Emit(const uint8_t* data, size_t len) {
    if (m_write && len > 0) m_write(data, len);
}

void WebMMuxer::Emit(const std::vector<uint8_t>& v) {
    Emit(v.data(), v.size());
}

// ─── Init ─────────────────────────────────────────────────────────────────────

void WebMMuxer::Init(VideoCodec codec, uint16_t width, uint16_t height,
                     uint32_t audioSampleRate, uint8_t audioChannels) {
    m_codec         = codec;
    m_width         = width;
    m_height        = height;
    m_audioRate     = audioSampleRate;
    m_audioChannels = audioChannels;
    m_clusterPts    = -1;
    m_initialized   = true;
    WriteEBMLHeader();
    WriteSegmentOpener();
}

// ─── EBML file header ─────────────────────────────────────────────────────────

void WebMMuxer::WriteEBMLHeader() {
    std::vector<uint8_t> body;
    app(body, elem_str(ID_DocType,            "webm"));
    app(body, elem_uint(ID_DocTypeVersion,    4));
    app(body, elem_uint(ID_DocTypeReadVersion, 2));
    Emit(elem(ID_EBML, body));
}

// ─── Segment (unknown-size) + Info + Tracks ───────────────────────────────────

void WebMMuxer::WriteSegmentOpener() {
    // Segment element with EBML "unknown size" = 0x01FFFFFFFFFFFFFF
    {
        std::vector<uint8_t> v;
        put_id(v, ID_Segment);
        // 8-byte VINT with all value bits set = indeterminate / unknown size
        v.push_back(0x01);
        for (int i = 0; i < 7; ++i) v.push_back(0xFF);
        Emit(v);
    }

    // ── Info ──────────────────────────────────────────────────────────────────
    // TimecodeScale = 1,000,000 ns  →  1 ms per timecode unit
    {
        std::vector<uint8_t> body;
        app(body, elem_uint(ID_TimecodeScale, 1000000ULL));
        app(body, elem_str(ID_MuxingApp,   "SwagLiveRecorder/WebMMuxer"));
        app(body, elem_str(ID_WritingApp,  "SwagLiveRecorder"));
        Emit(elem(ID_Info, body));
    }

    // ── Video track ───────────────────────────────────────────────────────────
    std::vector<uint8_t> te_video;
    {
        std::vector<uint8_t> vdim;
        app(vdim, elem_uint(ID_PixelWidth,  m_width));
        app(vdim, elem_uint(ID_PixelHeight, m_height));

        const std::string codecId =
            (m_codec == VideoCodec::VP8) ? "V_VP8" : "V_VP9";

        app(te_video, elem_uint(ID_TrackNumber, 1));
        app(te_video, elem_uint(ID_TrackUID,    1));
        app(te_video, elem_uint(ID_TrackType,   1));  // 1 = video
        app(te_video, elem_str(ID_CodecID, codecId));
        app(te_video, elem(ID_Video, vdim));
    }

    // ── Audio track (Opus) ────────────────────────────────────────────────────
    // OpusHead codec-private blob (19 bytes, fields are little-endian per Ogg/WebM Opus spec).
    // pre-skip = 312 samples (6.5 ms at 48 kHz)  →  312 = 0x0138 LE
    std::vector<uint8_t> te_audio;
    {
        const uint32_t sr = m_audioRate;
        std::vector<uint8_t> opusHead = {
            'O','p','u','s','H','e','a','d',   // magic
            1,                                 // version = 1
            m_audioChannels,                   // channel count
            0x38, 0x01,                        // pre-skip = 312 (LE uint16)
            static_cast<uint8_t>( sr        & 0xFF),
            static_cast<uint8_t>((sr >>  8) & 0xFF),
            static_cast<uint8_t>((sr >> 16) & 0xFF),
            static_cast<uint8_t>((sr >> 24) & 0xFF),  // input sample rate (LE uint32)
            0x00, 0x00,                        // output gain = 0 (LE int16)
            0x00                               // channel mapping family = 0
        };

        std::vector<uint8_t> adim;
        app(adim, elem_f64(ID_SamplingFreq, static_cast<double>(sr)));
        app(adim, elem_uint(ID_Channels, m_audioChannels));

        app(te_audio, elem_uint(ID_TrackNumber, 2));
        app(te_audio, elem_uint(ID_TrackUID,    2));
        app(te_audio, elem_uint(ID_TrackType,   2));  // 2 = audio
        app(te_audio, elem_str(ID_CodecID, "A_OPUS"));
        app(te_audio, elem(ID_CodecPrivate, opusHead));
        // CodecDelay = 6,500,000 ns (= 312 samples / 48000 Hz × 10^9)
        app(te_audio, elem_uint(ID_CodecDelay,  6500000ULL));
        // SeekPreRoll = 80,000,000 ns (80 ms – minimum Opus decode pre-roll)
        app(te_audio, elem_uint(ID_SeekPreRoll, 80000000ULL));
        app(te_audio, elem(ID_Audio, adim));
    }

    // ── Tracks element ────────────────────────────────────────────────────────
    {
        std::vector<uint8_t> tracks_body;
        app(tracks_body, elem(ID_TrackEntry, te_video));
        app(tracks_body, elem(ID_TrackEntry, te_audio));
        Emit(elem(ID_Tracks, tracks_body));
    }
}

// ─── Cluster management ───────────────────────────────────────────────────────

void WebMMuxer::OpenCluster(int64_t pts_ms) {
    m_clusterPts = pts_ms;

    // Cluster element with unknown size (streaming)
    std::vector<uint8_t> v;
    put_id(v, ID_Cluster);
    v.push_back(0x01);
    for (int i = 0; i < 7; ++i) v.push_back(0xFF);
    Emit(v);

    // Timecode – absolute, in ms (matching TimecodeScale = 1 ms)
    Emit(elem_uint(ID_Timecode, static_cast<uint64_t>(pts_ms)));
}

void WebMMuxer::MaybeRollCluster(int64_t pts_ms, bool forceNew) {
    if (m_clusterPts < 0) {
        OpenCluster(pts_ms);
        return;
    }
    int64_t elapsed = pts_ms - m_clusterPts;
    if (forceNew && elapsed >= 500) {
        OpenCluster(pts_ms);
    } else if (elapsed >= kClusterMs) {
        OpenCluster(pts_ms);
    }
}

// ─── SimpleBlock ──────────────────────────────────────────────────────────────

void WebMMuxer::WriteSimpleBlock(uint8_t trackNum, int64_t pts_ms, bool keyframe,
                                  const uint8_t* data, size_t len) {
    // Relative timecode from cluster start (signed int16, big-endian).
    int64_t rel64 = pts_ms - m_clusterPts;
    if (rel64 >  32767) rel64 =  32767;
    if (rel64 < -32768) rel64 = -32768;
    auto rel = static_cast<int16_t>(rel64);

    // Track number as 1-byte VINT (valid for track numbers 1–126).
    const uint8_t trackVint = static_cast<uint8_t>(0x80 | trackNum);
    const uint8_t relHi     = static_cast<uint8_t>(static_cast<uint16_t>(rel) >> 8);
    const uint8_t relLo     = static_cast<uint8_t>(static_cast<uint16_t>(rel) & 0xFF);
    // Flags: bit 7 = keyframe, bits 5-4 = lacing (00 = none), rest = 0
    const uint8_t flags     = keyframe ? 0x80 : 0x00;

    const size_t blockPayloadSize = 1 + 2 + 1 + len; // trackVint + rel + flags + data

    std::vector<uint8_t> block;
    block.reserve(1 + 8 + blockPayloadSize); // ID + max-size VINT + payload
    put_id(block, ID_SimpleBlock);
    put_size(block, blockPayloadSize);
    block.push_back(trackVint);
    block.push_back(relHi);
    block.push_back(relLo);
    block.push_back(flags);
    block.insert(block.end(), data, data + len);
    Emit(block);
}

// ─── Public write API ─────────────────────────────────────────────────────────

void WebMMuxer::WriteVideoFrame(const uint8_t* data, size_t len,
                                 int64_t pts_ms, bool keyframe) {
    if (!m_initialized || !data || len == 0) return;
    MaybeRollCluster(pts_ms, keyframe);
    WriteSimpleBlock(1, pts_ms, keyframe, data, len);
}

void WebMMuxer::WriteAudioPacket(const uint8_t* data, size_t len, int64_t pts_ms) {
    if (!m_initialized || !data || len == 0) return;
    // Audio never forces a new cluster; just ensure one is open.
    if (m_clusterPts < 0) OpenCluster(pts_ms);
    // Audio SimpleBlocks do not set the keyframe flag per Matroska spec §6.2.4.4
    WriteSimpleBlock(2, pts_ms, false, data, len);
}
