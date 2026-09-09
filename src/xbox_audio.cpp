#include "xbox_audio.h"

#include <cmath>

namespace {
constexpr float kTwoPi = 6.283185307179586f;
}

bool XboxAudio::submit_buffer(const int16_t* pcm, size_t frames) {
  if (!pcm || frames == 0) return false;
  if (buffers_.size() >= kMaxQueuedBuffers) return false;
  if (queued_frames() + frames > kMaxQueuedFrames) return false;
  buffers_.emplace_back(pcm, pcm + frames * kChannels);
  ++submitted_buffers_;
  return true;
}

bool XboxAudio::submit_buffer(const std::vector<int16_t>& interleaved_stereo) {
  if (interleaved_stereo.empty() || interleaved_stereo.size() % kChannels != 0)
    return false;
  return submit_buffer(interleaved_stereo.data(),
                       interleaved_stereo.size() / kChannels);
}

size_t XboxAudio::queued_frames() const {
  size_t n = 0;
  for (size_t i = 0; i < buffers_.size(); ++i) {
    const size_t frames = buffers_[i].size() / kChannels;
    n += (i == 0) ? frames - front_offset_frames_ : frames;
  }
  return n;
}

size_t XboxAudio::consume_frames(size_t frames) {
  size_t want = frames;
  size_t got = 0;
  while (want > 0 && !buffers_.empty()) {
    auto& front = buffers_.front();
    const size_t front_frames = front.size() / kChannels - front_offset_frames_;
    const size_t take = want < front_frames ? want : front_frames;
    front_offset_frames_ += take;
    want -= take;
    got += take;
    played_frames_ += take;
    if (front_offset_frames_ == front.size() / kChannels) {
      buffers_.erase(buffers_.begin());
      front_offset_frames_ = 0;
    }
  }
  underrun_frames_ += want;
  return got;
}

void XboxAudio::set_volume(float v) {
  if (v < 0.0f) v = 0.0f;
  if (v > 1.0f) v = 1.0f;
  volume_ = v;
}

void XboxAudio::reset() {
  buffers_.clear();
  front_offset_frames_ = 0;
  played_frames_ = 0;
  underrun_frames_ = 0;
  submitted_buffers_ = 0;
  volume_ = 1.0f;
}

std::vector<int16_t> XboxAudio::render_sine(float freq_hz, size_t frames,
                                            float& phase, float amplitude) {
  if (amplitude < 0.0f) amplitude = 0.0f;
  if (amplitude > 1.0f) amplitude = 1.0f;
  std::vector<int16_t> out;
  out.reserve(frames * kChannels);
  const float step = kTwoPi * freq_hz / static_cast<float>(kSampleRate);
  for (size_t i = 0; i < frames; ++i) {
    const float s = std::sin(phase) * amplitude * 32767.0f;
    const int16_t v = static_cast<int16_t>(s);
    out.push_back(v);  // L
    out.push_back(v);  // R
    phase += step;
    if (phase >= kTwoPi) phase -= kTwoPi;
  }
  return out;
}
