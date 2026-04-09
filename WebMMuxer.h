#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Minimal EBML/WebM muxer.  No external libraries required.
//
// Writes a valid streaming .webm file with:
//   track 1 – VP8 or VP9 video
//   track 2 – Opus audio
//
// Usage:
//   WebMMuxer mux([](const uint8_t* d, size_t n){ fwrite(d, 1, n, fp); });
//   mux.Init(WebMMuxer::VideoCodec::VP8, 1280, 720);
//   mux.WriteVideoFrame(frameData, frameLen, pts_ms, isKeyframe);
//   mux.WriteAudioPacket(opusData, opusLen, pts_ms);
//
// The output is a well-formed .webm that can be played by VLC, ffplay, etc.
// (No Duration or Cues are written; the Segment and every Cluster use the
// EBML "unknown size" marker, which is valid for streaming WebM.)

class WebMMuxer {
public:
    enum class VideoCodec { VP8, VP9 };

    // The muxer calls this every time it has bytes to write.
    using WriteCallback = std::function<void(const uint8_t* data, size_t len)>;

    explicit WebMMuxer(WriteCallback cb);

    // Must be called exactly once before any frames.
    // width / height      : video frame dimensions in pixels.
    // audioSampleRate     : almost always 48000 for WebRTC Opus.
    // audioChannels       : 1 (mono) or 2 (stereo).
    void Init(VideoCodec codec, uint16_t width, uint16_t height,
              uint32_t audioSampleRate = 48000, uint8_t audioChannels = 2);

    // pts_ms  : presentation timestamp in milliseconds (must be monotonically
    //           non-decreasing within each track; audio and video may interleave).
    // keyframe: true for VP8/VP9 intra-coded (I-frame) pictures.
    void WriteVideoFrame(const uint8_t* data, size_t len,
                         int64_t pts_ms, bool keyframe);

    // Pass the raw Opus payload from each RTP packet directly.
    void WriteAudioPacket(const uint8_t* data, size_t len, int64_t pts_ms);

private:
    WriteCallback m_write;
    VideoCodec    m_codec    = VideoCodec::VP8;
    uint16_t      m_width    = 0;
    uint16_t      m_height   = 0;
    uint32_t      m_audioRate = 48000;
    uint8_t       m_audioChannels = 2;
    int64_t       m_clusterPts   = -1;   // Timecode of the open cluster (-1 = none)
    bool          m_initialized  = false;

    // A new cluster is opened every kClusterMs ms, or immediately at a keyframe
    // that arrives more than 500 ms after the previous cluster start.
    static const int64_t kClusterMs = 2000;

    void Emit(const uint8_t* data, size_t len);
    void Emit(const std::vector<uint8_t>& v);
    void WriteEBMLHeader();
    void WriteSegmentOpener();   // Segment (unknown-size) + Info + Tracks
    void MaybeRollCluster(int64_t pts_ms, bool forceNew);
    void OpenCluster(int64_t pts_ms);
    void WriteSimpleBlock(uint8_t trackNum, int64_t pts_ms, bool keyframe,
                          const uint8_t* data, size_t len);
};
