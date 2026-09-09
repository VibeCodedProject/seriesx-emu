#pragma once
#include <cstdint>
#include <vector>

// XAudio2-lite voice HLE (homebrew only). One mastering voice at Xbox
// standard 48kHz stereo int16; titles submit PCM buffers, the backend
// consumes frames per update tick. Tracks queued/played accounting and
// underruns so titles can size their streaming buffers. A sine helper
// lets tests/demos render real samples without audio hardware.
struct XboxAudioFormat {
  uint32_t sample_rate = 48000;
  uint16_t channels = 2;
  uint16_t bits_per_sample = 16;
};

class XboxAudio {
 public:
  static constexpr uint32_t kSampleRate = 48000;
  static constexpr uint16_t kChannels = 2;
  static constexpr size_t kMaxQueuedBuffers = 16;
  // 2 seconds of stereo int16 cap (~384KB): big enough for streaming,
  // small enough to catch runaway submissions.
  static constexpr size_t kMaxQueuedFrames = 96000;

  const XboxAudioFormat& format() const { return format_; }

  // Submit one PCM buffer (frames = samples/2 channels). Rejects bad
  // format, empty, over-cap, or queue-full submissions.
  bool submit_buffer(const int16_t* pcm, size_t frames);
  bool submit_buffer(const std::vector<int16_t>& interleaved_stereo);

  // Consume up to `frames` for the next mix tick. Returns frames actually
  // consumed; shortfall counts as underrun frames.
  size_t consume_frames(size_t frames);

  void set_volume(float v);  // clamped 0..1
  float volume() const { return volume_; }

  size_t queued_buffers() const { return buffers_.size(); }
  size_t queued_frames() const;
  uint64_t played_frames() const { return played_frames_; }
  uint64_t underrun_frames() const { return underrun_frames_; }
  uint64_t submitted_buffers() const { return submitted_buffers_; }
  void reset();

  // Render a stereo sine at `freq_hz` for `frames` (phase-continuous if
  // `phase` is carried across calls). Amplitude 0..1 of int16 full scale.
  static std::vector<int16_t> render_sine(float freq_hz, size_t frames,
                                          float& phase, float amplitude = 0.5f);

 private:
  XboxAudioFormat format_{};
  std::vector<std::vector<int16_t>> buffers_;
  size_t front_offset_frames_ = 0;  // consumed frames in buffers_.front()
  float volume_ = 1.0f;
  uint64_t played_frames_ = 0;
  uint64_t underrun_frames_ = 0;
  uint64_t submitted_buffers_ = 0;
};
