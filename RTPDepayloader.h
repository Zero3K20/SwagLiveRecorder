#pragma once

// Single-header RTP depayloaders for VP8 and Opus.
// No external libraries required.
//
// VP8  – RFC 7741.  Accumulates RTP packets into complete VP8 frames.
// VP9  – RFC 9628.  Accumulates RTP packets into complete VP9 frames.
// Opus – RFC 7587.  Each RTP packet carries exactly one Opus packet.

#include <cstdint>
#include <functional>
#include <vector>

// ─── RTP header parser ────────────────────────────────────────────────────────

struct RTPHeader {
    bool     valid       = false;
    bool     marker      = false;   // M bit (end-of-frame for video)
    uint8_t  payloadType = 0;
    uint16_t sequence    = 0;
    uint32_t timestamp   = 0;
    uint32_t ssrc        = 0;
    size_t   headerBytes = 0;      // bytes consumed (fixed header + CSRC + extension)
};

//
//  RTP fixed header layout (RFC 3550 §5.1)
//
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |V=2|P|X|  CC   |M|     PT      |       sequence number         |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                           timestamp                           |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |           synchronization source (SSRC) identifier           |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

inline RTPHeader ParseRTPHeader(const uint8_t* data, size_t len) {
    RTPHeader h;
    if (len < 12) return h;
    if ((data[0] >> 6) != 2) return h;            // V must be 2

    bool    hasExt   = (data[0] >> 4) & 1;
    uint8_t csrcCnt  = data[0] & 0x0F;

    h.marker      = (data[1] >> 7) & 1;
    h.payloadType =  data[1] & 0x7F;
    h.sequence    = static_cast<uint16_t>((data[2] << 8) | data[3]);
    h.timestamp   = (static_cast<uint32_t>(data[4]) << 24) |
                    (static_cast<uint32_t>(data[5]) << 16) |
                    (static_cast<uint32_t>(data[6]) <<  8) |
                     static_cast<uint32_t>(data[7]);
    h.ssrc        = (static_cast<uint32_t>(data[ 8]) << 24) |
                    (static_cast<uint32_t>(data[ 9]) << 16) |
                    (static_cast<uint32_t>(data[10]) <<  8) |
                     static_cast<uint32_t>(data[11]);

    size_t off = 12 + 4u * csrcCnt;
    if (hasExt) {
        if (off + 4 > len) return h;
        uint16_t extWords = static_cast<uint16_t>((data[off + 2] << 8) | data[off + 3]);
        off += 4 + 4u * extWords;
    }
    if (off > len) return h;
    h.headerBytes = off;
    h.valid       = true;
    return h;
}

// ─── VP8 Depayloader (RFC 7741) ───────────────────────────────────────────────

class VP8Depayloader {
public:
    // Called with a complete, reassembled VP8 bitstream frame.
    // pts_ms   : presentation timestamp in milliseconds.
    // keyframe : true when the frame is an intra (key) frame.
    using FrameCallback = std::function<void(const uint8_t* data, size_t len,
                                             int64_t pts_ms, bool keyframe)>;
    FrameCallback onFrame;

    // Feed one complete RTP packet (including the RTP header).
    void Push(const uint8_t* rtpData, size_t rtpLen) {
        auto hdr = ParseRTPHeader(rtpData, rtpLen);
        if (!hdr.valid) return;

        const uint8_t* payload    = rtpData + hdr.headerBytes;
        size_t         payloadLen = rtpLen  - hdr.headerBytes;
        if (payloadLen == 0) return;

        // ── VP8 payload descriptor (RFC 7741 §4.2) ───────────────────────────
        // Byte 0:   X=ext, R, N=non-ref, S=start-of-partition, PID=partition-id(3b)
        // If X:
        //   Byte 1: I=PictureID, L=tl0picidx, T=tid, K=keyidx  (M=res, unused here)
        // If I:     1 or 2 bytes of PictureID (M-bit in picture-ID byte selects 2B)
        // If L:     1 byte tl0picidx
        // If T||K:  1 byte (TID / KEYIDX)

        size_t off = 0;
        bool   X   = (payload[0] >> 7) & 1;
        bool   S   = (payload[0] >> 4) & 1;  // start-of-partition
        uint8_t PID = payload[0] & 0x07;      // partition index
        off = 1;

        bool I = false, L = false, T = false, K = false;
        if (X) {
            if (off >= payloadLen) return;
            I = (payload[off] >> 7) & 1;
            L = (payload[off] >> 6) & 1;
            T = (payload[off] >> 5) & 1;
            K = (payload[off] >> 4) & 1;
            ++off;
        }
        if (I) {
            if (off >= payloadLen) return;
            bool M = (payload[off] >> 7) & 1;   // 16-bit PictureID flag
            off += M ? 2 : 1;
        }
        if (L && off < payloadLen) ++off;       // tl0picidx
        if ((T || K) && off < payloadLen) ++off; // TID / KEYIDX

        if (off >= payloadLen) return;

        const uint8_t* vp8 = payload + off;
        size_t         vp8len = payloadLen - off;

        // First packet of a new frame: S=1, PID=0
        if (S && PID == 0) {
            m_buf.clear();
            m_pts_ms  = static_cast<int64_t>(hdr.timestamp) * 1000 / 90000;
            // VP8 frame-tag bit 0: 0 = keyframe, 1 = inter frame
            m_keyframe = (vp8len >= 3) && ((vp8[0] & 0x01) == 0);
        }

        m_buf.insert(m_buf.end(), vp8, vp8 + vp8len);

        // M-bit set = last RTP packet of this frame → dispatch
        if (hdr.marker && !m_buf.empty() && onFrame) {
            onFrame(m_buf.data(), m_buf.size(), m_pts_ms, m_keyframe);
            m_buf.clear();
        }
    }

private:
    std::vector<uint8_t> m_buf;
    int64_t              m_pts_ms  = 0;
    bool                 m_keyframe = false;
};

// ─── VP9 Depayloader (RFC 9628) ───────────────────────────────────────────────

class VP9Depayloader {
public:
    using FrameCallback = std::function<void(const uint8_t* data, size_t len,
                                             int64_t pts_ms, bool keyframe)>;
    FrameCallback onFrame;

    // Feed one complete RTP packet (including the RTP header).
    void Push(const uint8_t* rtpData, size_t rtpLen) {
        auto hdr = ParseRTPHeader(rtpData, rtpLen);
        if (!hdr.valid) return;

        const uint8_t* payload    = rtpData + hdr.headerBytes;
        size_t         payloadLen = rtpLen  - hdr.headerBytes;
        if (payloadLen == 0) return;

        // ── VP9 payload descriptor (RFC 9628 §4) ─────────────────────────────
        // Byte 0: I P L F B E V Z
        //  I = picture ID present
        //  P = inter-picture predicted layer frame (P=0 → keyframe)
        //  L = layer indices present
        //  F = flexible mode
        //  B = start of VP9 layer frame
        //  E = end of VP9 layer frame
        //  V = scalability structure data present
        //  Z = not a reference frame

        size_t off = 0;
        if (off >= payloadLen) return;

        bool I = (payload[0] >> 7) & 1;
        bool P = (payload[0] >> 6) & 1;   // P=0 means keyframe / intra
        bool L = (payload[0] >> 5) & 1;
        bool F = (payload[0] >> 4) & 1;
        bool B = (payload[0] >> 3) & 1;   // start of frame
        bool E = (payload[0] >> 2) & 1;   // end of frame
        bool V = (payload[0] >> 1) & 1;
        off = 1;

        if (I) {   // PictureID
            if (off >= payloadLen) return;
            bool M = (payload[off] >> 7) & 1;  // 15-bit PictureID
            off += M ? 2 : 1;
        }
        if (L) {   // Layer indices
            if (off + 1 >= payloadLen) return;
            off += 2;
            if (!F) {                          // reference indices only in non-flexible mode
                if (off >= payloadLen) return;
                ++off;
            }
        }
        if (F && P) { // Reference indices in flexible mode
            // Up to 3 reference picture IDs (each 1 or 2 bytes)
            for (int r = 0; r < 3 && off < payloadLen; ++r) {
                bool N = payload[off] & 0x01;
                off += N ? 2 : 1;
                if (!N) break;
            }
        }
        if (V) {   // Scalability structure
            if (off >= payloadLen) return;
            uint8_t ns = ((payload[off] >> 5) & 0x07) + 1; // N_S+1 spatial layers
            bool Y = (payload[off] >> 4) & 1;
            bool G = (payload[off] >> 3) & 1;
            ++off;
            for (uint8_t s = 0; s < ns && off < payloadLen; ++s) {
                if (Y) off += 4; // width[s], height[s] (2×uint16)
            }
            if (G && off < payloadLen) {
                uint8_t ng = payload[off++];   // N_G
                for (uint8_t g = 0; g < ng && off < payloadLen; ++g) {
                    uint8_t nr = (payload[off] & 0x0C) >> 2; // N_R refs
                    ++off;
                    off += nr; // R[i]
                }
            }
        }

        if (off >= payloadLen) return;

        const uint8_t* vp9    = payload + off;
        size_t         vp9len = payloadLen - off;

        if (B) { // beginning of a new frame
            m_buf.clear();
            m_pts_ms   = static_cast<int64_t>(hdr.timestamp) * 1000 / 90000;
            m_keyframe = !P;  // P=0 → not inter-predicted → keyframe
        }

        m_buf.insert(m_buf.end(), vp9, vp9 + vp9len);

        if (E && !m_buf.empty() && onFrame) {
            onFrame(m_buf.data(), m_buf.size(), m_pts_ms, m_keyframe);
            m_buf.clear();
        }
    }

private:
    std::vector<uint8_t> m_buf;
    int64_t              m_pts_ms  = 0;
    bool                 m_keyframe = false;
};

// ─── Opus Depayloader (RFC 7587) ─────────────────────────────────────────────
//
// No framing or reassembly needed: every RTP packet carries exactly one
// complete Opus packet.  The payload IS the Opus bitstream; no descriptor.

class OpusDepayloader {
public:
    // pts_ms is derived from the RTP timestamp (48 kHz clock).
    using PacketCallback = std::function<void(const uint8_t* data, size_t len,
                                              int64_t pts_ms)>;
    PacketCallback onPacket;

    void Push(const uint8_t* rtpData, size_t rtpLen) {
        auto hdr = ParseRTPHeader(rtpData, rtpLen);
        if (!hdr.valid) return;

        const uint8_t* payload    = rtpData + hdr.headerBytes;
        size_t         payloadLen = rtpLen  - hdr.headerBytes;
        if (payloadLen == 0) return;

        // Opus RTP clock is 48000 Hz per RFC 7587 §4.
        int64_t pts_ms = static_cast<int64_t>(hdr.timestamp) * 1000 / 48000;

        if (onPacket) onPacket(payload, payloadLen, pts_ms);
    }
};
