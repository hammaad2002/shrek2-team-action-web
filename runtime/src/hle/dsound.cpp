// dsound.dll: IDirectSound8 / IDirectSoundBuffer8 / IDirectSound3DBuffer8 / IDirectSound3DListener8
// on top of the software mixer. Buffer memory lives in guest memory, so Lock() hands out real
// guest pointers and the mixer reads what the game writes.
#include <cmath>
#include <mutex>

#include "../audio/mixer.h"
#include "../core/objects.h"
#include "com.h"

using namespace rt;

namespace {

const Guid IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const Guid IID_IDirectSoundBuffer = {0x279AFA85, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const Guid IID_IDirectSoundBuffer8 = {0x6825A449, 0x7524, 0x4D82, {0x92, 0x0F, 0x50, 0xE3, 0x6A, 0xB3, 0xAB, 0x1E}};
const Guid IID_IDirectSound3DBuffer = {0x279AFA86, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const Guid IID_IDirectSound3DListener = {0x279AFA84, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

constexpr uint32_t DSBCAPS_PRIMARYBUFFER = 0x1, DSBCAPS_CTRL3D = 0x10;
constexpr uint32_t DSERR_INVALIDPARAM = 0x80070057, DSERR_NOINTERFACE = 0x80004002;

struct Vec3 {
  float x = 0, y = 0, z = 0;
};

struct Listener {
  Vec3 pos, front{0, 0, 1}, top{0, 1, 0};
  float rolloff = 1.0f, distance_factor = 1.0f;
};
Listener g_listener;
std::mutex g_listener_mutex;

struct BufferData {  // shared by duplicated buffers
  uint32_t mem = 0;
  uint32_t size = 0;
};

class SoundBuffer : public ComObject, public audio::Source {
 public:
  const char* type_name() const override { return "DirectSoundBuffer"; }
  std::shared_ptr<BufferData> data;
  bool primary = false, ctrl3d = false;
  uint16_t channels = 2, bits = 16, block_align = 4;
  uint32_t rate = 22050, freq = 0;
  int32_t volume = 0, pan = 0;
  // 3D parameters (DS3DMODE_NORMAL = 0, HEADRELATIVE = 1, DISABLE = 2).
  Vec3 pos;
  float min_dist = 1.0f, max_dist = 1e9f;
  uint32_t mode3d = 0;
  std::mutex m;
  std::atomic<bool> playing{false}, looping{false};
  double cursor = 0;  // in sample frames
  std::shared_ptr<audio::Source> self_ref;

  uint32_t frames() const { return block_align ? data->size / block_align : 0; }

  void gains(float* l, float* r) {
    float g = audio::db100_to_gain(volume);
    float pl = pan > 0 ? audio::db100_to_gain(-pan) : 1.0f;
    float pr = pan < 0 ? audio::db100_to_gain(pan) : 1.0f;
    if (ctrl3d && mode3d != 2) {
      std::lock_guard<std::mutex> lock(g_listener_mutex);
      Vec3 rel = mode3d == 1 ? pos : Vec3{pos.x - g_listener.pos.x, pos.y - g_listener.pos.y, pos.z - g_listener.pos.z};
      float d = std::sqrt(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z) * g_listener.distance_factor;
      float att = d <= min_dist ? 1.0f : min_dist / (min_dist + g_listener.rolloff * (std::min(d, max_dist) - min_dist));
      g *= att;
      if (d > 1e-4f && mode3d != 1) {
        // Project onto the listener's right vector (front x top) for a simple stereo pan.
        Vec3 f = g_listener.front, t = g_listener.top;
        Vec3 right{t.y * f.z - t.z * f.y, t.z * f.x - t.x * f.z, t.x * f.y - t.y * f.x};
        float rl = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
        float s = rl > 0 ? (rel.x * right.x + rel.y * right.y + rel.z * right.z) / (rl * d / g_listener.distance_factor) : 0;
        s = std::clamp(s, -1.0f, 1.0f);
        pl *= std::sqrt(0.5f * (1 - s)) * 1.41421f;
        pr *= std::sqrt(0.5f * (1 + s)) * 1.41421f;
      }
    }
    *l = g * std::min(pl, 1.0f);
    *r = g * std::min(pr, 1.0f);
  }

  float sample(uint32_t frame, int ch) const {
    uint32_t off = data->mem + frame * block_align + (uint32_t)ch * (bits / 8);
    if (bits == 16) return (float)(int16_t)ld16(off) / 32768.0f;
    return ((float)ld8(off) - 128.0f) / 128.0f;
  }

  void mix(float* out, int n) override {
    if (!playing || primary) return;
    std::lock_guard<std::mutex> lock(m);
    uint32_t total = frames();
    if (!total) return;
    float gl, gr;
    gains(&gl, &gr);
    double step = (double)(freq ? freq : rate) / audio::kRate;
    for (int i = 0; i < n; i++) {
      uint32_t f0 = (uint32_t)cursor;
      uint32_t f1 = f0 + 1 < total ? f0 + 1 : (looping ? 0 : f0);
      float t = (float)(cursor - f0);
      float l = sample(f0, 0) * (1 - t) + sample(f1, 0) * t;
      float r = channels > 1 ? sample(f0, 1) * (1 - t) + sample(f1, 1) * t : l;
      out[2 * i] += l * gl;
      out[2 * i + 1] += r * gr;
      cursor += step;
      if (cursor >= total) {
        if (looping) cursor = std::fmod(cursor, (double)total);
        else {
          cursor = 0;
          playing = false;
          return;
        }
      }
    }
  }
};

class DirectSound : public ComObject {
 public:
  const char* type_name() const override { return "DirectSound8"; }
};

class Sound3DListener : public ComObject {
 public:
  const char* type_name() const override { return "DirectSound3DListener"; }
};

ComInterface& buffer_iface();
ComInterface& buffer3d_iface();
ComInterface& listener_iface();

Vec3 read_vec(uint32_t a) { return {bits2f32(ld32(a)), bits2f32(ld32(a + 4)), bits2f32(ld32(a + 8))}; }
void write_vec(uint32_t a, const Vec3& v) {
  st32(a, f32bits(v.x));
  st32(a + 4, f32bits(v.y));
  st32(a + 8, f32bits(v.z));
}

// ---- IDirectSound8 ---------------------------------------------------------------------------

uint32_t ds_CreateSoundBuffer(Cpu* c, ComObject*) {
  uint32_t desc = arg(c, 1), out = arg(c, 2);
  uint32_t flags = ld32(desc + 4), bytes = ld32(desc + 8), wfx = ld32(desc + 16);
  auto* b = new SoundBuffer();
  b->primary = (flags & DSBCAPS_PRIMARYBUFFER) != 0;
  b->ctrl3d = (flags & DSBCAPS_CTRL3D) != 0;
  b->data = std::make_shared<BufferData>();
  if (!b->primary) {
    if (!wfx || !bytes) {
      delete b;
      return DSERR_INVALIDPARAM;
    }
    b->channels = (uint16_t)ld16(wfx + 2);
    b->rate = ld32(wfx + 4);
    b->block_align = (uint16_t)ld16(wfx + 12);
    b->bits = (uint16_t)ld16(wfx + 14);
    b->data->size = bytes;
    b->data->mem = heap_alloc(process_heap(), bytes, true);
    if (b->bits == 8) memset(host(b->data->mem), 0x80, bytes);
    std::shared_ptr<SoundBuffer> sp(b, [](SoundBuffer*) {});  // lifetime owned by COM refcount
    b->self_ref = sp;
    audio::add(sp);
  }
  st32(out, com_wrap(b, buffer_iface()));
  return S_OK;
}

uint32_t ds_DuplicateSoundBuffer(Cpu* c, ComObject*) {
  auto* src = com_as<SoundBuffer>(arg(c, 1));
  if (!src) return DSERR_INVALIDPARAM;
  auto* b = new SoundBuffer();
  b->data = src->data;
  b->ctrl3d = src->ctrl3d;
  b->channels = src->channels;
  b->rate = src->rate;
  b->bits = src->bits;
  b->block_align = src->block_align;
  b->volume = src->volume;
  b->pan = src->pan;
  b->freq = src->freq;
  std::shared_ptr<SoundBuffer> sp(b, [](SoundBuffer*) {});
  b->self_ref = sp;
  audio::add(sp);
  st32(arg(c, 2), com_wrap(b, buffer_iface()));
  return S_OK;
}

uint32_t ds_GetCaps(Cpu* c, ComObject*) {
  uint32_t caps = arg(c, 1);
  uint32_t size = ld32(caps);
  memset(host(caps + 4), 0, size > 4 ? size - 4 : 0);
  st32(caps + 4, 0x00000f5f);  // DSCAPS_PRIMARY*/SECONDARY* | CONTINUOUSRATE
  st32(caps + 8, 100);         // dwMinSecondarySampleRate
  st32(caps + 12, 100000);     // dwMaxSecondarySampleRate
  st32(caps + 16, 1);          // dwPrimaryBuffers
  for (uint32_t o = 20; o <= 48; o += 4) st32(caps + o, 256);  // hardware buffer counts
  return S_OK;
}

uint32_t ok(Cpu*, ComObject*) { return S_OK; }

uint32_t ds_GetSpeakerConfig(Cpu* c, ComObject*) {
  st32(arg(c, 1), 4);  // DSSPEAKER_STEREO
  return S_OK;
}

ComInterface& dsound_iface() {
  static ComInterface i("IDirectSound8", {{"QueryInterface", 2, com_QueryInterface_none},
                                          {"AddRef", 0, com_AddRef},
                                          {"Release", 0, com_Release},
                                          {"CreateSoundBuffer", 3, ds_CreateSoundBuffer},
                                          {"GetCaps", 1, ds_GetCaps},
                                          {"DuplicateSoundBuffer", 2, ds_DuplicateSoundBuffer},
                                          {"SetCooperativeLevel", 2, ok},
                                          {"Compact", 0, ok},
                                          {"GetSpeakerConfig", 1, ds_GetSpeakerConfig},
                                          {"SetSpeakerConfig", 1, ok},
                                          {"Initialize", 1, ok},
                                          {"VerifyCertification", 1, nullptr}});
  return i;
}

// ---- IDirectSoundBuffer8 ---------------------------------------------------------------------

SoundBuffer* buf(ComObject* o) { return static_cast<SoundBuffer*>(o); }

uint32_t buf_QueryInterface(Cpu* c, ComObject* self) {
  Guid iid = read_guid(arg(c, 1));
  uint32_t out = arg(c, 2);
  auto* b = buf(self);
  if (guid_eq(iid, IID_IDirectSoundBuffer8) || guid_eq(iid, IID_IDirectSoundBuffer) || guid_eq(iid, IID_IUnknown)) {
    self->refs++;
    st32(out, arg(c, 0));
    return S_OK;
  }
  if (guid_eq(iid, IID_IDirectSound3DBuffer) && b->ctrl3d) {
    self->refs++;
    st32(out, com_wrap(self, buffer3d_iface()));
    return S_OK;
  }
  if (guid_eq(iid, IID_IDirectSound3DListener) && b->primary) {
    st32(out, com_wrap(new Sound3DListener(), listener_iface()));
    return S_OK;
  }
  log(Log::Warn, "IDirectSoundBuffer::QueryInterface(%s) not supported", guid_str(iid).c_str());
  st32(out, 0);
  return DSERR_NOINTERFACE;
}

uint32_t buf_Release(Cpu* c, ComObject* self) {
  auto* b = buf(self);
  if (b->refs.load() == 1) {
    b->playing = false;
    audio::remove(b);
    if (b->data && b->data.use_count() == 1 && b->data->mem) heap_free(process_heap(), b->data->mem);
  }
  return com_Release(c, self);
}

uint32_t buf_GetCaps(Cpu* c, ComObject* self) {
  uint32_t caps = arg(c, 1);
  st32(caps + 4, 0x000000e0 | (buf(self)->ctrl3d ? 0x10u : 0u));
  st32(caps + 8, buf(self)->data->size);
  st32(caps + 12, 0);
  st32(caps + 16, 0);
  return S_OK;
}

uint32_t buf_GetCurrentPosition(Cpu* c, ComObject* self) {
  auto* b = buf(self);
  std::lock_guard<std::mutex> lock(b->m);
  uint32_t play = (uint32_t)b->cursor * b->block_align;
  uint32_t ahead = (b->freq ? b->freq : b->rate) / 100 * b->block_align;  // ~10 ms write lead
  uint32_t write = b->data->size ? (play + ahead) % b->data->size : 0;
  if (arg(c, 1)) st32(arg(c, 1), play);
  if (arg(c, 2)) st32(arg(c, 2), write);
  return S_OK;
}

uint32_t buf_GetFormat(Cpu* c, ComObject* self) {
  auto* b = buf(self);
  uint32_t wfx = arg(c, 1), size = arg(c, 2), written = arg(c, 3);
  if (wfx && size >= 16) {
    st16(wfx, 1);
    st16(wfx + 2, b->channels);
    st32(wfx + 4, b->rate);
    st32(wfx + 8, b->rate * b->block_align);
    st16(wfx + 12, b->block_align);
    st16(wfx + 14, b->bits);
    if (size >= 18) st16(wfx + 16, 0);
  }
  if (written) st32(written, 18);
  return S_OK;
}

uint32_t buf_GetVolume(Cpu* c, ComObject* self) {
  st32(arg(c, 1), (uint32_t)buf(self)->volume);
  return S_OK;
}
uint32_t buf_GetPan(Cpu* c, ComObject* self) {
  st32(arg(c, 1), (uint32_t)buf(self)->pan);
  return S_OK;
}
uint32_t buf_GetFrequency(Cpu* c, ComObject* self) {
  st32(arg(c, 1), buf(self)->freq ? buf(self)->freq : buf(self)->rate);
  return S_OK;
}
uint32_t buf_GetStatus(Cpu* c, ComObject* self) {
  auto* b = buf(self);
  st32(arg(c, 1), (b->playing ? 1u : 0u) | (b->playing && b->looping ? 4u : 0u));
  return S_OK;
}

uint32_t buf_Lock(Cpu* c, ComObject* self) {
  auto* b = buf(self);
  uint32_t off = arg(c, 1), bytes = arg(c, 2), p1 = arg(c, 3), n1 = arg(c, 4), p2 = arg(c, 5), n2 = arg(c, 6), flags = arg(c, 7);
  uint32_t size = b->data->size;
  if (b->primary || !size) return DSERR_INVALIDPARAM;
  if (flags & 1) {  // DSBLOCK_FROMWRITECURSOR
    std::lock_guard<std::mutex> lock(b->m);
    off = ((uint32_t)b->cursor * b->block_align + (b->rate / 100) * b->block_align) % size;
  }
  if (flags & 2) bytes = size;  // DSBLOCK_ENTIREBUFFER
  off %= size;
  bytes = std::min(bytes, size);
  uint32_t first = std::min(bytes, size - off);
  st32(p1, b->data->mem + off);
  st32(n1, first);
  if (p2) st32(p2, bytes > first ? b->data->mem : 0);
  if (n2) st32(n2, bytes - first);
  return S_OK;
}

uint32_t buf_Play(Cpu* c, ComObject* self) {
  auto* b = buf(self);
  b->looping = (arg(c, 3) & 1) != 0;
  b->playing = true;
  return S_OK;
}

uint32_t buf_SetCurrentPosition(Cpu* c, ComObject* self) {
  auto* b = buf(self);
  std::lock_guard<std::mutex> lock(b->m);
  b->cursor = b->block_align ? (double)(arg(c, 1) / b->block_align) : 0;
  return S_OK;
}

uint32_t buf_SetVolume(Cpu* c, ComObject* self) {
  buf(self)->volume = (int32_t)arg(c, 1);
  return S_OK;
}
uint32_t buf_SetPan(Cpu* c, ComObject* self) {
  buf(self)->pan = (int32_t)arg(c, 1);
  return S_OK;
}
uint32_t buf_SetFrequency(Cpu* c, ComObject* self) {
  buf(self)->freq = arg(c, 1);
  return S_OK;
}
uint32_t buf_Stop(Cpu* c, ComObject* self) {
  buf(self)->playing = false;
  return S_OK;
}

ComInterface& buffer_iface() {
  static ComInterface i("IDirectSoundBuffer8", {{"QueryInterface", 2, buf_QueryInterface},
                                                {"AddRef", 0, com_AddRef},
                                                {"Release", 0, buf_Release},
                                                {"GetCaps", 1, buf_GetCaps},
                                                {"GetCurrentPosition", 2, buf_GetCurrentPosition},
                                                {"GetFormat", 3, buf_GetFormat},
                                                {"GetVolume", 1, buf_GetVolume},
                                                {"GetPan", 1, buf_GetPan},
                                                {"GetFrequency", 1, buf_GetFrequency},
                                                {"GetStatus", 1, buf_GetStatus},
                                                {"Initialize", 2, ok},
                                                {"Lock", 7, buf_Lock},
                                                {"Play", 3, buf_Play},
                                                {"SetCurrentPosition", 1, buf_SetCurrentPosition},
                                                {"SetFormat", 1, ok},
                                                {"SetVolume", 1, buf_SetVolume},
                                                {"SetPan", 1, buf_SetPan},
                                                {"SetFrequency", 1, buf_SetFrequency},
                                                {"Stop", 0, buf_Stop},
                                                {"Unlock", 4, ok},
                                                {"Restore", 0, ok},
                                                {"SetFX", 3, nullptr},
                                                {"AcquireResources", 3, nullptr},
                                                {"GetObjectInPath", 4, nullptr}});
  return i;
}

// ---- IDirectSound3DBuffer8 -------------------------------------------------------------------

uint32_t b3_QueryInterface(Cpu* c, ComObject* self) { return buf_QueryInterface(c, self); }
uint32_t b3_Release(Cpu* c, ComObject* self) { return buf_Release(c, self); }
uint32_t b3_SetPosition(Cpu* c, ComObject* self) {
  buf(self)->pos = {bits2f32(arg(c, 1)), bits2f32(arg(c, 2)), bits2f32(arg(c, 3))};
  return S_OK;
}
uint32_t b3_GetPosition(Cpu* c, ComObject* self) {
  write_vec(arg(c, 1), buf(self)->pos);
  return S_OK;
}
uint32_t b3_SetMinDistance(Cpu* c, ComObject* self) {
  buf(self)->min_dist = bits2f32(arg(c, 1));
  return S_OK;
}
uint32_t b3_SetMaxDistance(Cpu* c, ComObject* self) {
  buf(self)->max_dist = bits2f32(arg(c, 1));
  return S_OK;
}
uint32_t b3_GetMinDistance(Cpu* c, ComObject* self) {
  st32(arg(c, 1), f32bits(buf(self)->min_dist));
  return S_OK;
}
uint32_t b3_GetMaxDistance(Cpu* c, ComObject* self) {
  st32(arg(c, 1), f32bits(buf(self)->max_dist));
  return S_OK;
}
uint32_t b3_SetMode(Cpu* c, ComObject* self) {
  buf(self)->mode3d = arg(c, 1);
  return S_OK;
}
uint32_t b3_GetMode(Cpu* c, ComObject* self) {
  st32(arg(c, 1), buf(self)->mode3d);
  return S_OK;
}
uint32_t b3_SetAllParameters(Cpu* c, ComObject* self) {
  uint32_t p = arg(c, 1);  // DS3DBUFFER: size, position, velocity, cone..., min, max, mode
  auto* b = buf(self);
  b->pos = read_vec(p + 4);
  b->min_dist = bits2f32(ld32(p + 52));
  b->max_dist = bits2f32(ld32(p + 56));
  b->mode3d = ld32(p + 60);
  return S_OK;
}

ComInterface& buffer3d_iface() {
  static ComInterface i("IDirectSound3DBuffer8", {{"QueryInterface", 2, b3_QueryInterface},
                                                  {"AddRef", 0, com_AddRef},
                                                  {"Release", 0, b3_Release},
                                                  {"GetAllParameters", 1, nullptr},
                                                  {"GetConeAngles", 2, nullptr},
                                                  {"GetConeOrientation", 1, nullptr},
                                                  {"GetConeOutsideVolume", 1, nullptr},
                                                  {"GetMaxDistance", 1, b3_GetMaxDistance},
                                                  {"GetMinDistance", 1, b3_GetMinDistance},
                                                  {"GetMode", 1, b3_GetMode},
                                                  {"GetPosition", 1, b3_GetPosition},
                                                  {"GetVelocity", 1, nullptr},
                                                  {"SetAllParameters", 2, b3_SetAllParameters},
                                                  {"SetConeAngles", 3, ok},
                                                  {"SetConeOrientation", 4, ok},
                                                  {"SetConeOutsideVolume", 2, ok},
                                                  {"SetMaxDistance", 2, b3_SetMaxDistance},
                                                  {"SetMinDistance", 2, b3_SetMinDistance},
                                                  {"SetMode", 2, b3_SetMode},
                                                  {"SetPosition", 4, b3_SetPosition},
                                                  {"SetVelocity", 4, ok}});
  return i;
}

// ---- IDirectSound3DListener8 -----------------------------------------------------------------

uint32_t l_SetPosition(Cpu* c, ComObject*) {
  std::lock_guard<std::mutex> lock(g_listener_mutex);
  g_listener.pos = {bits2f32(arg(c, 1)), bits2f32(arg(c, 2)), bits2f32(arg(c, 3))};
  return S_OK;
}
uint32_t l_SetOrientation(Cpu* c, ComObject*) {
  std::lock_guard<std::mutex> lock(g_listener_mutex);
  g_listener.front = {bits2f32(arg(c, 1)), bits2f32(arg(c, 2)), bits2f32(arg(c, 3))};
  g_listener.top = {bits2f32(arg(c, 4)), bits2f32(arg(c, 5)), bits2f32(arg(c, 6))};
  return S_OK;
}
uint32_t l_SetRolloffFactor(Cpu* c, ComObject*) {
  std::lock_guard<std::mutex> lock(g_listener_mutex);
  g_listener.rolloff = bits2f32(arg(c, 1));
  return S_OK;
}
uint32_t l_SetDistanceFactor(Cpu* c, ComObject*) {
  std::lock_guard<std::mutex> lock(g_listener_mutex);
  g_listener.distance_factor = bits2f32(arg(c, 1));
  return S_OK;
}
uint32_t l_GetPosition(Cpu* c, ComObject*) {
  std::lock_guard<std::mutex> lock(g_listener_mutex);
  write_vec(arg(c, 1), g_listener.pos);
  return S_OK;
}

ComInterface& listener_iface() {
  static ComInterface i("IDirectSound3DListener8", {{"QueryInterface", 2, com_QueryInterface_none},
                                                    {"AddRef", 0, com_AddRef},
                                                    {"Release", 0, com_Release},
                                                    {"GetAllParameters", 1, nullptr},
                                                    {"GetDistanceFactor", 1, nullptr},
                                                    {"GetDopplerFactor", 1, nullptr},
                                                    {"GetOrientation", 2, nullptr},
                                                    {"GetPosition", 1, l_GetPosition},
                                                    {"GetRolloffFactor", 1, nullptr},
                                                    {"GetVelocity", 1, nullptr},
                                                    {"SetAllParameters", 2, nullptr},
                                                    {"SetDistanceFactor", 2, l_SetDistanceFactor},
                                                    {"SetDopplerFactor", 2, ok},
                                                    {"SetOrientation", 7, l_SetOrientation},
                                                    {"SetPosition", 4, l_SetPosition},
                                                    {"SetRolloffFactor", 2, l_SetRolloffFactor},
                                                    {"SetVelocity", 4, ok},
                                                    {"CommitDeferredSettings", 0, ok}});
  return i;
}

}  // namespace

// DirectSoundCreate8 is imported by ordinal 11.
HLE_STDCALL("dsound.dll", DirectSoundCreate8, 3) {
  audio::init();
  st32(arg(c, 1), com_wrap(new DirectSound(), dsound_iface()));
  log(Log::Info, "DirectSoundCreate8 -> software mixer");
  return S_OK;
}
static rt::HleRegistrar g_ds_ordinal("dsound.dll", "#11", [](Cpu* c) { ret_stdcall(c, hle_impl_DirectSoundCreate8(c), 3); });
