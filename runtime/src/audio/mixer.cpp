#include "mixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "../platform/platform.h"

namespace audio {
namespace {
std::mutex g_mutex;
std::vector<std::shared_ptr<Source>> g_sources;
std::atomic<uint64_t> g_frames{0};
}  // namespace

void init() {
  static bool opened = false;
  if (!opened) opened = plat::audio_open(kRate, render);
}

void add(const std::shared_ptr<Source>& s) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_sources.push_back(s);
}

void remove(Source* s) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_sources.erase(std::remove_if(g_sources.begin(), g_sources.end(), [&](auto& p) { return p.get() == s; }), g_sources.end());
}

void render(float* out, int frames) {
  memset(out, 0, sizeof(float) * 2 * (size_t)frames);
  {
    // Held while mixing so a source cannot be destroyed mid-mix (remove() takes the same lock).
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& s : g_sources) s->mix(out, frames);
  }
  for (int i = 0; i < frames * 2; i++) out[i] = std::clamp(out[i], -1.0f, 1.0f);
  g_frames += (uint64_t)frames;
}

uint64_t frames_rendered() { return g_frames.load(); }

float db100_to_gain(int32_t v) {
  if (v <= -10000) return 0.0f;
  if (v >= 0) return 1.0f;
  return std::pow(10.0f, (float)v / 2000.0f);
}

}  // namespace audio
