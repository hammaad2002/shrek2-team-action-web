// binkw32.dll HLE: Bink 1 playback on FFmpeg's libavcodec decoders.
//
// The game uses Bink for the intro videos (decoded into a locked D3D texture with
// BinkCopyToBuffer) and for streamed music and voice (audio-only files opened from memory with
// BINKFROMMEMORY). We parse the BIK container ourselves (header, frame index, per-frame audio
// packets followed by the video packet) and hand packets to libavcodec. Audio goes to the
// runtime mixer; when a file has audio, playback timing follows the audio clock.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../audio/mixer.h"
#include "../core/objects.h"

using namespace rt;

namespace {

constexpr uint32_t BINKFROMMEMORY = 0x04000000;
constexpr uint32_t kBinkStructSize = 0x400;
constexpr double kAudioLead = 0.5;  // seconds of audio decoded ahead of the playback clock

// Byte source for a BIK file: guest memory (BINKFROMMEMORY) or a host file.
class BikData {
 public:
  bool open_file(const std::string& host_path) {
    fp_ = fopen(host_path.c_str(), "rb");
    if (!fp_) return false;
    fseek(fp_, 0, SEEK_END);
    size_ = (uint64_t)ftell(fp_);
    return true;
  }
  void open_memory(const uint8_t* p, uint64_t size) {
    mem_ = p;
    size_ = size;
  }
  ~BikData() {
    if (fp_) fclose(fp_);
  }
  bool read(uint64_t off, void* dst, uint32_t n) {
    if (off + n > size_) return false;
    if (mem_) {
      memcpy(dst, mem_ + off, n);
      return true;
    }
    fseek(fp_, (long)off, SEEK_SET);
    return fread(dst, 1, n, fp_) == n;
  }
  uint64_t size() const { return size_; }
  void set_size(uint64_t s) { size_ = s; }

 private:
  FILE* fp_ = nullptr;
  const uint8_t* mem_ = nullptr;
  uint64_t size_ = 0;
};

uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// Decoded audio waiting to be played, consumed by the mixer at the device rate.
class BinkAudio : public audio::Source {
 public:
  int rate = 22050, channels = 2;
  std::mutex m;
  std::deque<float> q;              // interleaved stereo
  uint64_t written = 0;             // source frames queued since open
  std::atomic<uint64_t> consumed{0};  // source frames played since open
  std::atomic<bool> paused{false};
  std::atomic<float> gain{1.0f};
  double frac = 0;

  void push(const float* lr, size_t frames) {
    std::lock_guard<std::mutex> lock(m);
    q.insert(q.end(), lr, lr + frames * 2);
    written += frames;
  }
  void flush() {
    std::lock_guard<std::mutex> lock(m);
    consumed += q.size() / 2;  // dropped frames count as played so the clock stays consistent
    q.clear();
    frac = 0;
  }
  uint64_t queued() {
    std::lock_guard<std::mutex> lock(m);
    return q.size() / 2;
  }

  void mix(float* out, int frames) override {
    if (paused) return;
    std::lock_guard<std::mutex> lock(m);
    double step = (double)rate / audio::kRate;
    float gn = gain;
    for (int i = 0; i < frames; i++) {
      if (q.size() < 4) break;  // underrun: wait for more data
      double f = frac;
      out[2 * i] += gn * (float)(q[0] + (q[2] - q[0]) * f);
      out[2 * i + 1] += gn * (float)(q[1] + (q[3] - q[1]) * f);
      frac += step;
      while (frac >= 1.0 && q.size() >= 4) {
        frac -= 1.0;
        q.pop_front();
        q.pop_front();
        consumed++;
      }
    }
  }
};

struct Bink {
  uint32_t guest = 0;  // BINK struct in guest memory
  BikData data;
  uint32_t signature = 0, frames = 0, width = 0, height = 0, fps_num = 30, fps_den = 1, video_flags = 0, largest = 0;
  std::vector<uint32_t> index;  // frames + 1 offsets (bit 0 = keyframe)
  uint32_t ntracks = 0;
  std::vector<uint16_t> track_rate, track_flags;
  uint32_t frame_num = 1;  // 1-based current frame (BINK.FrameNum)
  uint32_t last_frame = 0;
  bool paused = false;

  // Video.
  AVCodecContext* vctx = nullptr;
  AVFrame* vframe = nullptr;
  AVFrame* scratch = nullptr;
  uint32_t decoded_frame = 0;  // frame held in vframe (0 = none)
  bool have_picture = false;

  // Audio (first track).
  AVCodecContext* actx = nullptr;
  AVFrame* aframe = nullptr;
  std::shared_ptr<BinkAudio> audio;
  uint32_t audio_cursor = 1;  // next frame whose audio is decoded
  uint64_t base_consumed = 0;
  double base_time = 0;

  // Wall clock (files without audio).
  std::chrono::steady_clock::time_point wall_base;
  double wall_base_time = 0;
  double paused_at = 0;

  double frame_time(uint32_t n) const { return (double)(n - 1) * fps_den / fps_num; }
  bool keyframe(uint32_t n) const { return n >= 1 && n <= frames && (index[n - 1] & 1); }
  uint64_t frame_pos(uint32_t n) const { return index[n - 1] & ~1u; }
  uint32_t frame_size(uint32_t n) const { return (uint32_t)((index[n] & ~1u) - (index[n - 1] & ~1u)); }

  double clock() {
    if (audio) {
      return base_time + (double)(audio->consumed.load() - base_consumed) / audio->rate;
    }
    if (paused) return paused_at;
    return wall_base_time + std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_base).count();
  }

  bool read_frame(uint32_t n, std::vector<uint8_t>& buf) {
    if (n < 1 || n > frames) return false;
    buf.resize(frame_size(n) + AV_INPUT_BUFFER_PADDING_SIZE);
    std::fill(buf.end() - AV_INPUT_BUFFER_PADDING_SIZE, buf.end(), 0);
    if (!data.read(frame_pos(n), buf.data(), frame_size(n))) return false;
    buf.resize(frame_size(n));
    return true;
  }

  // Decode the audio packet of frame n (track 0) into the playback queue.
  void decode_audio(uint32_t n) {
    std::vector<uint8_t> buf;
    if (!read_frame(n, buf) || buf.size() < 4) return;
    uint32_t asize = rd32(buf.data());
    if (asize < 4 || asize > buf.size() - 4) return;
    AVPacket* pkt = av_packet_alloc();
    if (av_new_packet(pkt, (int)asize) < 0) {
      av_packet_free(&pkt);
      return;
    }
    memcpy(pkt->data, buf.data() + 4, asize);
    if (avcodec_send_packet(actx, pkt) == 0) {
      std::vector<float> lr;
      while (avcodec_receive_frame(actx, aframe) == 0) {
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        int ch = aframe->ch_layout.nb_channels;
#else
        int ch = aframe->channels;
#endif
        int ns = aframe->nb_samples;
        lr.resize((size_t)ns * 2);
        for (int i = 0; i < ns; i++) {
          float l, r;
          if (aframe->format == AV_SAMPLE_FMT_FLTP) {
            l = ((const float*)aframe->extended_data[0])[i];
            r = ch > 1 ? ((const float*)aframe->extended_data[1])[i] : l;
          } else if (aframe->format == AV_SAMPLE_FMT_FLT) {
            const float* s = (const float*)aframe->data[0];
            l = s[i * ch];
            r = ch > 1 ? s[i * ch + 1] : l;
          } else if (aframe->format == AV_SAMPLE_FMT_S16) {
            const int16_t* s = (const int16_t*)aframe->data[0];
            l = s[i * ch] / 32768.0f;
            r = ch > 1 ? s[i * ch + 1] / 32768.0f : l;
          } else {
            l = r = 0;
          }
          lr[2 * (size_t)i] = l;
          lr[2 * (size_t)i + 1] = r;
        }
        audio->push(lr.data(), (size_t)ns);
      }
    }
    av_packet_free(&pkt);
  }

  // Keep kAudioLead seconds of audio decoded ahead of the playback position.
  void fill_audio() {
    if (!audio) return;
    while (audio_cursor <= frames && audio->queued() < (uint64_t)(kAudioLead * audio->rate)) decode_audio(audio_cursor++);
  }

  // Jump playback to frame n. `flush` drops audio already queued (a real seek); otherwise the
  // queued audio plays out first (seamless loop back to the start).
  void seek(uint32_t n, bool flush) {
    n = std::clamp<uint32_t>(n, 1, std::max<uint32_t>(frames, 1));
    frame_num = n;
    if (audio) {
      if (flush) {
        audio->flush();
        base_consumed = audio->consumed.load();
      } else {
        std::lock_guard<std::mutex> lock(audio->m);
        base_consumed = audio->written;
      }
      base_time = frame_time(n);
      audio_cursor = n;
      avcodec_flush_buffers(actx);
      fill_audio();
    } else {
      wall_base = std::chrono::steady_clock::now();
      wall_base_time = paused_at = frame_time(n);
    }
  }

  // Make vframe hold frame n.
  bool decode_video(uint32_t n) {
    if (!vctx || n < 1 || n > frames) return false;
    if (decoded_frame == n && have_picture) return true;
    uint32_t start = decoded_frame + 1;
    if (decoded_frame == 0 || n < start || n - start > 64) {
      start = n;
      while (start > 1 && !keyframe(start)) start--;
      avcodec_flush_buffers(vctx);
    }
    std::vector<uint8_t> buf;
    for (uint32_t f = start; f <= n; f++) {
      if (!read_frame(f, buf)) return false;
      // Skip the audio packets of every track.
      size_t off = 0;
      for (uint32_t t = 0; t < ntracks && off + 4 <= buf.size(); t++) off += 4 + rd32(buf.data() + off);
      if (off > buf.size()) return false;
      AVPacket* pkt = av_packet_alloc();
      int vsize = (int)(buf.size() - off);
      if (av_new_packet(pkt, vsize) < 0) {
        av_packet_free(&pkt);
        return false;
      }
      memcpy(pkt->data, buf.data() + off, (size_t)vsize);
      if (keyframe(f)) pkt->flags |= AV_PKT_FLAG_KEY;
      int r = avcodec_send_packet(vctx, pkt);
      av_packet_free(&pkt);
      if (r < 0) {
        log(Log::Warn, "Bink: video decode error %d at frame %u", r, f);
        continue;
      }
      // A failing avcodec_receive_frame() unrefs its output, so receive into a scratch frame.
      int got = 0;
      if (!scratch) scratch = av_frame_alloc();
      while ((r = avcodec_receive_frame(vctx, scratch)) == 0) {
        av_frame_unref(vframe);
        av_frame_move_ref(vframe, scratch);
        have_picture = true;
        got++;
      }
      if (trace_enabled("bink"))
        log(Log::Debug, "Bink: frame %u (%d bytes, key %d) -> %d picture(s), last receive %d, fmt %d, %dx%d, Y[center] %d", f, vsize,
            keyframe(f), got, r, vframe->format, vframe->width, vframe->height,
            vframe->data[0] ? vframe->data[0][(size_t)(vframe->height / 2) * (size_t)vframe->linesize[0] + (size_t)vframe->width / 2] : -1);
      decoded_frame = f;
    }
    return have_picture;
  }

  void update_struct() {
    uint32_t b = guest;
    st32(b + 0x00, width);
    st32(b + 0x04, height);
    st32(b + 0x08, frames);
    st32(b + 0x0c, frame_num);
    st32(b + 0x10, last_frame);
    st32(b + 0x14, fps_num);
    st32(b + 0x18, fps_den);
  }

  ~Bink() {
    if (audio) audio::remove(audio.get());
    if (vctx) avcodec_free_context(&vctx);
    if (actx) avcodec_free_context(&actx);
    if (vframe) av_frame_free(&vframe);
    if (scratch) av_frame_free(&scratch);
    if (aframe) av_frame_free(&aframe);
  }
};

std::mutex g_mutex;
std::unordered_map<uint32_t, std::unique_ptr<Bink>> g_binks;

Bink* get(uint32_t guest) {
  auto it = g_binks.find(guest);
  return it == g_binks.end() ? nullptr : it->second.get();
}

void set_extradata(AVCodecContext* ctx, uint32_t value) {
  ctx->extradata = (uint8_t*)av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE);
  memcpy(ctx->extradata, &value, 4);
  ctx->extradata_size = 4;
}

bool parse_header(Bink* b) {
  uint8_t h[44];
  if (!b->data.read(0, h, sizeof h)) return false;
  b->signature = rd32(h);
  if ((b->signature & 0xffffff) != ('B' | 'I' << 8 | 'K' << 16)) return false;
  uint64_t file_size = (uint64_t)rd32(h + 4) + 8;
  if (b->data.size() == 0 || b->data.size() > file_size) b->data.set_size(file_size);
  b->frames = rd32(h + 8);
  b->largest = rd32(h + 12);
  b->width = rd32(h + 20);
  b->height = rd32(h + 24);
  b->fps_num = rd32(h + 28);
  b->fps_den = rd32(h + 32);
  b->video_flags = rd32(h + 36);
  b->ntracks = rd32(h + 40);
  if (!b->fps_num || !b->fps_den) b->fps_num = 30, b->fps_den = 1;
  if (b->frames == 0 || b->frames > 1000000 || b->ntracks > 256) return false;
  uint64_t off = 44 + 4ull * b->ntracks;
  for (uint32_t t = 0; t < b->ntracks; t++) {
    uint8_t tr[4];
    if (!b->data.read(off + 4ull * t, tr, 4)) return false;
    b->track_rate.push_back((uint16_t)(tr[0] | tr[1] << 8));
    b->track_flags.push_back((uint16_t)(tr[2] | tr[3] << 8));
  }
  off += 8ull * b->ntracks;  // track formats and track ids
  b->index.resize(b->frames + 1);
  std::vector<uint8_t> idx(4ull * b->frames);
  if (!b->data.read(off, idx.data(), (uint32_t)idx.size())) return false;
  for (uint32_t i = 0; i < b->frames; i++) b->index[i] = rd32(&idx[4ull * i]);
  b->index[b->frames] = (uint32_t)b->data.size();
  return true;
}

void open_decoders(Bink* b) {
  if (b->width && b->height) {
    const AVCodec* vc = avcodec_find_decoder(AV_CODEC_ID_BINKVIDEO);
    if (vc) {
      b->vctx = avcodec_alloc_context3(vc);
      b->vctx->codec_tag = b->signature;
      b->vctx->width = (int)b->width;
      b->vctx->height = (int)b->height;
      set_extradata(b->vctx, b->video_flags);
      if (avcodec_open2(b->vctx, vc, nullptr) < 0) {
        log(Log::Warn, "Bink: cannot open the video decoder");
        avcodec_free_context(&b->vctx);
      }
      b->vframe = av_frame_alloc();
    }
  }
  if (b->ntracks) {
    bool dct = b->track_flags[0] & 0x1000;
    bool stereo = b->track_flags[0] & 0x2000;
    const AVCodec* ac = avcodec_find_decoder(dct ? AV_CODEC_ID_BINKAUDIO_DCT : AV_CODEC_ID_BINKAUDIO_RDFT);
    if (ac) {
      b->actx = avcodec_alloc_context3(ac);
      b->actx->sample_rate = b->track_rate[0];
      b->actx->codec_tag = 0;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
      av_channel_layout_default(&b->actx->ch_layout, stereo ? 2 : 1);
#else
      b->actx->channels = stereo ? 2 : 1;
      b->actx->channel_layout = stereo ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
#endif
      set_extradata(b->actx, b->signature);
      if (avcodec_open2(b->actx, ac, nullptr) < 0) {
        log(Log::Warn, "Bink: cannot open the audio decoder");
        avcodec_free_context(&b->actx);
      } else {
        b->aframe = av_frame_alloc();
        b->audio = std::make_shared<BinkAudio>();
        b->audio->rate = b->track_rate[0];
        audio::init();
        audio::add(b->audio);
      }
    }
  }
}

// BT.601 limited-range YUV 4:2:0 to the Bink surface formats.
void copy_to_buffer(Bink* b, uint32_t dest, uint32_t pitch, uint32_t dest_h, uint32_t dx, uint32_t dy, uint32_t flags) {
  AVFrame* f = b->vframe;
  uint32_t type = flags & 0xf;
  uint32_t w = std::min<uint32_t>(b->width, (uint32_t)f->width), h = std::min<uint32_t>({b->height, (uint32_t)f->height, dest_h > dy ? dest_h - dy : 0});
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t* Y = f->data[0] + (size_t)y * (size_t)f->linesize[0];
    const uint8_t* U = f->data[1] + (size_t)(y / 2) * (size_t)f->linesize[1];
    const uint8_t* V = f->data[2] + (size_t)(y / 2) * (size_t)f->linesize[2];
    const uint8_t* A = f->format == AV_PIX_FMT_YUVA420P ? f->data[3] + (size_t)y * (size_t)f->linesize[3] : nullptr;
    uint8_t* row = host(dest + (dy + y) * pitch);
    for (uint32_t x = 0; x < w; x++) {
      int c = ((int)Y[x] - 16) * 298, d = (int)U[x / 2] - 128, e = (int)V[x / 2] - 128;
      int r = std::clamp((c + 409 * e + 128) >> 8, 0, 255);
      int g = std::clamp((c - 100 * d - 208 * e + 128) >> 8, 0, 255);
      int bl = std::clamp((c + 516 * d + 128) >> 8, 0, 255);
      uint8_t a = A ? A[x] : 255;
      uint32_t X = dx + x;
      switch (type) {
        case 1:  // 24-bit BGR
          row[3 * X] = (uint8_t)bl, row[3 * X + 1] = (uint8_t)g, row[3 * X + 2] = (uint8_t)r;
          break;
        case 2:  // 24-bit RGB
          row[3 * X] = (uint8_t)r, row[3 * X + 1] = (uint8_t)g, row[3 * X + 2] = (uint8_t)bl;
          break;
        case 4:  // 32-bit RGBx
        case 6:
          row[4 * X] = (uint8_t)r, row[4 * X + 1] = (uint8_t)g, row[4 * X + 2] = (uint8_t)bl, row[4 * X + 3] = a;
          break;
        case 7: {  // 4444
          uint16_t v = (uint16_t)((a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | bl >> 4);
          memcpy(row + 2 * X, &v, 2);
          break;
        }
        case 8:
        case 9: {  // 5551 / 555
          uint16_t v = (uint16_t)((type == 8 && a < 128 ? 0 : 0x8000) | (r >> 3) << 10 | (g >> 3) << 5 | bl >> 3);
          memcpy(row + 2 * X, &v, 2);
          break;
        }
        case 10: {  // 565
          uint16_t v = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | bl >> 3);
          memcpy(row + 2 * X, &v, 2);
          break;
        }
        default:  // 3/5: 32-bit BGRx / BGRA
          row[4 * X] = (uint8_t)bl, row[4 * X + 1] = (uint8_t)g, row[4 * X + 2] = (uint8_t)r, row[4 * X + 3] = a;
          break;
      }
    }
  }
}

}  // namespace

HLE_STDCALL_AS("binkw32.dll", BinkOpenDirectSound, "_BinkOpenDirectSound@4", 1) { return 0; }

// The sound system argument is the address of BinkOpenDirectSound; audio always goes to our mixer.
HLE_STDCALL_AS("binkw32.dll", BinkSetSoundSystem, "_BinkSetSoundSystem@8", 2) { return 1; }

HLE_STDCALL_AS("binkw32.dll", BinkOpen, "_BinkOpen@8", 2) {
  uint32_t name = arg(c, 0), flags = arg(c, 1);
  auto b = std::make_unique<Bink>();
  std::string what;
  if (flags & BINKFROMMEMORY) {
    // The file is already in guest memory; its size comes from the header.
    uint32_t size = ld32(name + 4) + 8;
    b->data.open_memory(host(name), size);
    char buf[32];
    snprintf(buf, sizeof buf, "memory@%08x", name);
    what = buf;
  } else {
    what = read_cstr(name);
    std::string hp = vfs_host_path(what, false);
    if (hp.empty() || !b->data.open_file(hp)) {
      log(Log::Warn, "BinkOpen(%s): file not found", what.c_str());
      return 0;
    }
  }
  if (!parse_header(b.get())) {
    log(Log::Warn, "BinkOpen(%s): not a Bink 1 file", what.c_str());
    return 0;
  }
  open_decoders(b.get());
  b->guest = heap_alloc(process_heap(), kBinkStructSize, true);
  st32(b->guest + 0x20, flags);  // OpenFlags
  b->wall_base = std::chrono::steady_clock::now();
  b->fill_audio();
  b->update_struct();
  log(Log::Info, "BinkOpen(%s, %08x): %ux%u, %u frames at %.2f fps, %u audio track(s)%s", what.c_str(), flags, b->width, b->height, b->frames,
      (double)b->fps_num / b->fps_den, b->ntracks, b->ntracks ? (b->track_flags[0] & 0x2000 ? " stereo" : " mono") : "");
  uint32_t g = b->guest;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_binks[g] = std::move(b);
  return g;
}

HLE_STDCALL_AS("binkw32.dll", BinkClose, "_BinkClose@4", 1) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_binks.find(arg(c, 0));
  if (it != g_binks.end()) {
    heap_free(process_heap(), it->first);
    g_binks.erase(it);
  }
  return 0;
}

HLE_STDCALL_AS("binkw32.dll", BinkWait, "_BinkWait@4", 1) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Bink* b = get(arg(c, 0));
  if (!b) return 0;
  b->fill_audio();
  if (b->paused) return 1;
  return b->clock() < b->frame_time(b->frame_num) ? 1 : 0;
}

HLE_STDCALL_AS("binkw32.dll", BinkDoFrame, "_BinkDoFrame@4", 1) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Bink* b = get(arg(c, 0));
  if (!b) return 0;
  b->fill_audio();
  b->last_frame = b->frame_num;
  b->update_struct();
  return 0;
}

HLE_STDCALL_AS("binkw32.dll", BinkNextFrame, "_BinkNextFrame@4", 1) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Bink* b = get(arg(c, 0));
  if (!b) return 0;
  if (b->frame_num >= b->frames) b->seek(1, false);  // Bink loops back to the first frame
  else b->frame_num++;
  b->fill_audio();
  b->update_struct();
  return 0;
}

HLE_STDCALL_AS("binkw32.dll", BinkGoto, "_BinkGoto@12", 3) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Bink* b = get(arg(c, 0));
  if (!b) return 0;
  uint32_t frame = std::max<uint32_t>(arg(c, 1), 1);
  // Jumping back to the start once all audio has been decoded is a loop: let the tail play out.
  bool loop = b->audio && b->audio_cursor > b->frames && frame == 1;
  b->seek(frame, !loop);
  b->update_struct();
  return 0;
}

HLE_STDCALL_AS("binkw32.dll", BinkPause, "_BinkPause@8", 2) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Bink* b = get(arg(c, 0));
  if (!b) return 0;
  bool p = arg(c, 1) != 0;
  if (p != b->paused) {
    if (p) b->paused_at = b->clock();
    else {
      b->wall_base = std::chrono::steady_clock::now();
      b->wall_base_time = b->paused_at;
    }
    b->paused = p;
    if (b->audio) b->audio->paused = p;
  }
  return p ? 1 : 0;
}

// Volume: 32768 is unity gain (up to 65536).
HLE_STDCALL_AS("binkw32.dll", BinkSetVolume, "_BinkSetVolume@12", 3) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Bink* b = get(arg(c, 0));
  if (b && b->audio) b->audio->gain = (float)std::min<uint32_t>(arg(c, 2), 65536) / 32768.0f;
  return 0;
}

HLE_STDCALL_AS("binkw32.dll", BinkService, "_BinkService@4", 1) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (Bink* b = get(arg(c, 0))) b->fill_audio();
  return 0;
}

HLE_STDCALL_AS("binkw32.dll", BinkCopyToBuffer, "_BinkCopyToBuffer@28", 7) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Bink* b = get(arg(c, 0));
  if (!b || !b->decode_video(b->last_frame ? b->last_frame : b->frame_num)) return 0;
  copy_to_buffer(b, arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6));
  return 0;
}
