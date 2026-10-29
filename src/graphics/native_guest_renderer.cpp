#include <rex/graphics/native_guest_renderer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/logging.h>

// Defined here (shared TU) rather than in a backend's command_processor.cpp
// so both D3D12 and Vulkan see the same definition.
REXCVAR_DEFINE_BOOL(native_render_suppress_emulated_draws, true, "GPU",
                    "While the registered native guest-output renderer is actively "
                    "replacing frames, skip emulated draw and resolve execution in the "
                    "command processor (PM4 parsing, fences, queries and memexport draws "
                    "still run). Menus/pause yield to the emulated path and execute "
                    "normally.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(native_render_suppress_mode, 2, "GPU",
                     "Which emulated passes to suppress while the native guest-output "
                     "renderer is active. 0 = framebuffer-sized passes only (surface "
                     "pitch >= 1280); the game's whole postfx chain then still executes "
                     "at 1152x640 x resolution scale EVERY frame and paces the pipeline "
                     "(~half the achievable frame rate). 1 = suppress everything (perf "
                     "probing; mid-gameplay lightmap page composition breaks). 2 = "
                     "suppress everything EXCEPT the memory-composition passes whose "
                     "outputs the native renderer samples from guest memory: lightmap "
                     "page composition (pitch 1024) and small composite surfaces (pitch "
                     "<= 512, CAS outfit pieces). Menus/pause/loading always render "
                     "fully (the native renderer yields there), so shop/outfit "
                     "composition is unaffected by any mode. 3 = portrait-window mode: "
                     "like 0 the sub-framebuffer RTT passes execute (one-shot frontend "
                     "portrait renders, census pitches 560-1200), but the 1152-wide "
                     "main scene + postfx band stays suppressed; that band is the "
                     "whole-pipeline cost at scaled resolutions and portraits never "
                     "need it.")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(native_render_cull_unconsumed_passes, true, "GPU",
                    "While emulated draws are suppressed, also skip the draws and "
                    "resolves of per-frame emulated passes whose output the native "
                    "renderer never samples (the game's shadow blur/convert, "
                    "world-shade map, bloom mips). One-shot passes (lightmap page "
                    "composition, portraits) and sampled passes always execute.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(native_render_suppress_exempt_depth_only, true, "GPU",
                    "Within the suppression-EXEMPT passes (see "
                    "native_render_suppress_mode), also skip draws with no pixel shader "
                    "(depth/stencil-only: shadow-map casters, z-prepasses). Their output "
                    "feeds only the suppressed scene passes; the native renderer builds "
                    "its own shadows, and on Vulkan this stream is the dominant "
                    "remaining emulated GPU cost (2-12 ms/frame at 3x, the bimodal-FPS "
                    "slow state). Disable if lightmap-page or CAS composition content "
                    "regresses (a composition pass depth/stencil-testing against its own "
                    "no-PS lay-down would need this off).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics {
namespace {

std::atomic<NativeGuestOutputRenderer> g_renderer{nullptr};
std::atomic<void*> g_renderer_user_data{nullptr};
// True while the registered renderer actually replaced the last presented
// frame (false when it yields: menus, early-outs, no renderer).
std::atomic<bool> g_native_output_active{false};
std::atomic<NativeGuestOutputPostProcessor> g_post_processor{nullptr};
std::atomic<void*> g_post_processor_user_data{nullptr};
std::atomic<bool> g_post_process_requested{false};

}  // namespace

void SetNativeGuestOutputRenderer(NativeGuestOutputRenderer renderer, void* user_data) {
  g_renderer_user_data.store(user_data, std::memory_order_release);
  g_renderer.store(renderer, std::memory_order_release);
}

bool TryRenderNativeGuestOutput(const NativeGuestOutputRenderContext& context) {
  NativeGuestOutputRenderer renderer = g_renderer.load(std::memory_order_acquire);
  if (renderer == nullptr) {
    g_native_output_active.store(false, std::memory_order_relaxed);
    return false;
  }
  void* user_data = g_renderer_user_data.load(std::memory_order_acquire);
  const bool rendered = renderer(context, user_data);
  g_native_output_active.store(rendered, std::memory_order_relaxed);
  return rendered;
}

bool HasNativeGuestOutputRenderer() {
  return g_renderer.load(std::memory_order_acquire) != nullptr;
}

namespace {
NativeInterFrameHooks g_inter_frame_hooks;
std::atomic<bool> g_inter_frame_hooks_set{false};
thread_local bool tl_in_inter_frame = false;
}  // namespace

void SetNativeInterFrameHooks(const NativeInterFrameHooks& hooks) {
  g_inter_frame_hooks_set.store(false, std::memory_order_release);
  g_inter_frame_hooks = hooks;
  g_inter_frame_hooks_set.store(hooks.want != nullptr, std::memory_order_release);
}

const NativeInterFrameHooks* GetNativeInterFrameHooks() {
  return g_inter_frame_hooks_set.load(std::memory_order_acquire) ? &g_inter_frame_hooks
                                                                 : nullptr;
}

bool IsNativeInterFrame() {
  return tl_in_inter_frame;
}

namespace {
std::mutex g_swap_rate_mutex;
std::array<int64_t, 256> g_swap_times_us{};
uint32_t g_swap_times_next = 0;
uint32_t g_swap_times_count = 0;
std::atomic<int64_t> g_last_inter_frame_us{0};

int64_t SwapRateNowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

void NoteNativeGuestSwap() {
  const int64_t now = SwapRateNowUs();
  std::lock_guard<std::mutex> lock(g_swap_rate_mutex);
  g_swap_times_us[g_swap_times_next] = now;
  g_swap_times_next = (g_swap_times_next + 1) % uint32_t(g_swap_times_us.size());
  g_swap_times_count = std::min<uint32_t>(g_swap_times_count + 1, uint32_t(g_swap_times_us.size()));
}

void NoteNativeInterFramePresented() {
  g_last_inter_frame_us.store(SwapRateNowUs(), std::memory_order_relaxed);
}

double NativeGuestSwapRate() {
  const int64_t now = SwapRateNowUs();
  std::lock_guard<std::mutex> lock(g_swap_rate_mutex);
  // Swaps within the last second, newest first.
  int64_t newest = 0, oldest = 0;
  uint32_t n = 0;
  for (uint32_t k = 0; k < g_swap_times_count; ++k) {
    const uint32_t i =
        (g_swap_times_next + uint32_t(g_swap_times_us.size()) - 1 - k) % uint32_t(g_swap_times_us.size());
    const int64_t t = g_swap_times_us[i];
    if (now - t > 1000000) {
      break;
    }
    if (n == 0) {
      newest = t;
    }
    oldest = t;
    ++n;
  }
  if (n < 2 || newest <= oldest) {
    return 0.0;
  }
  return double(n - 1) * 1e6 / double(newest - oldest);
}

bool NativeInterFramesRecent() {
  const int64_t last = g_last_inter_frame_us.load(std::memory_order_relaxed);
  return last != 0 && SwapRateNowUs() - last < 1000000;
}

bool TryRenderNativeInterFrame(const NativeGuestOutputRenderContext& context) {
  NativeGuestOutputRenderer renderer = g_renderer.load(std::memory_order_acquire);
  if (renderer == nullptr) {
    return false;
  }
  void* user_data = g_renderer_user_data.load(std::memory_order_acquire);
  tl_in_inter_frame = true;
  const bool rendered = renderer(context, user_data);
  tl_in_inter_frame = false;
  return rendered;
}

namespace {
NativeResolveJobHooks g_resolve_job_hooks;
std::atomic<bool> g_resolve_job_hooks_set{false};
}  // namespace

void SetNativeResolveJobHooks(const NativeResolveJobHooks& hooks) {
  g_resolve_job_hooks = hooks;
  g_resolve_job_hooks_set.store(hooks.claim != nullptr && hooks.render != nullptr &&
                                    hooks.complete != nullptr,
                                std::memory_order_release);
}

const NativeResolveJobHooks* GetNativeResolveJobHooks() {
  return g_resolve_job_hooks_set.load(std::memory_order_acquire) ? &g_resolve_job_hooks
                                                                 : nullptr;
}

void SetNativeGuestOutputPostProcessor(NativeGuestOutputPostProcessor post_processor,
                                       void* user_data) {
  g_post_processor_user_data.store(user_data, std::memory_order_release);
  g_post_processor.store(post_processor, std::memory_order_release);
}

bool HasNativeGuestOutputPostProcessor() {
  return g_post_processor.load(std::memory_order_acquire) != nullptr;
}

void InvokeNativeGuestOutputPostProcessor(const NativeGuestOutputRenderContext& context) {
  NativeGuestOutputPostProcessor post_processor =
      g_post_processor.load(std::memory_order_acquire);
  if (post_processor == nullptr) {
    return;
  }
  post_processor(context, g_post_processor_user_data.load(std::memory_order_acquire));
}

void RequestNativeGuestOutputPostProcess(bool requested) {
  g_post_process_requested.store(requested, std::memory_order_release);
}

bool IsNativeGuestOutputPostProcessRequested() {
  return g_post_process_requested.load(std::memory_order_acquire);
}

bool IsNativeGuestOutputActive() {
  return g_native_output_active.load(std::memory_order_relaxed);
}

namespace {
std::atomic<NativeGuestOutputSubmitHook> g_submit_hook{nullptr};
std::atomic<void*> g_submit_hook_user_data{nullptr};
}  // namespace

void SetNativeGuestOutputSubmitHook(NativeGuestOutputSubmitHook hook, void* user_data) {
  g_submit_hook_user_data.store(user_data, std::memory_order_release);
  g_submit_hook.store(hook, std::memory_order_release);
}

void InvokeNativeGuestOutputSubmitHook(NativeGuestOutputBackend backend) {
  NativeGuestOutputSubmitHook hook = g_submit_hook.load(std::memory_order_acquire);
  if (hook == nullptr) {
    return;
  }
  hook(backend, g_submit_hook_user_data.load(std::memory_order_acquire));
}

namespace {
std::atomic<double> g_wide_aspect{0.0};
}  // namespace

void SetNativeGuestOutputWideAspect(double aspect) {
  g_wide_aspect.store(aspect > 0.0 ? aspect : 0.0, std::memory_order_relaxed);
}

double GetNativeGuestOutputWideAspect() {
  return g_wide_aspect.load(std::memory_order_relaxed);
}

bool ApplyNativeGuestOutputWideAspect(uint32_t& guest_output_width, uint32_t guest_output_height,
                                      uint32_t& display_width, uint32_t& display_height) {
  const double aspect = g_wide_aspect.load(std::memory_order_relaxed);
  if (aspect <= 0.0 || !guest_output_width || !guest_output_height ||
      !HasNativeGuestOutputRenderer() ||
      !g_native_output_active.load(std::memory_order_relaxed)) {
    return false;
  }
  // Even width keeps every half-resolution derived target (bloom, SSAO,
  // menu snapshots) an exact divide.
  uint32_t wide_width =
      uint32_t(std::clamp(aspect * double(guest_output_height) + 0.5, 1.0, 16384.0)) & ~1u;
  if (wide_width <= guest_output_width) {
    return false;
  }
  guest_output_width = wide_width;
  // The display aspect is the actual pixel aspect: the presenter maps the
  // wide image edge-to-edge on a matching window.
  display_width = guest_output_width;
  display_height = guest_output_height;
  return true;
}

static std::atomic<NativeGuestOutputSizeProvider> g_size_provider{nullptr};
static std::atomic<void*> g_size_provider_user{nullptr};

void SetNativeGuestOutputSizeProvider(NativeGuestOutputSizeProvider provider, void* user_data) {
  g_size_provider_user.store(user_data, std::memory_order_relaxed);
  g_size_provider.store(provider, std::memory_order_relaxed);
}

bool ApplyNativeGuestOutputSize(uint32_t& guest_output_width, uint32_t& guest_output_height,
                                uint32_t& display_width, uint32_t& display_height) {
  const NativeGuestOutputSizeProvider provider =
      g_size_provider.load(std::memory_order_relaxed);
  if (provider == nullptr || !HasNativeGuestOutputRenderer() ||
      !g_native_output_active.load(std::memory_order_relaxed)) {
    return false;
  }
  uint32_t width = 0;
  uint32_t height = 0;
  if (!provider(width, height, g_size_provider_user.load(std::memory_order_relaxed))) {
    return false;
  }
  // Even dimensions keep every half-resolution derived target an exact divide.
  width = std::min<uint32_t>(width, 16384u) & ~1u;
  height = std::min<uint32_t>(height, 16384u) & ~1u;
  if (width < 2 || height < 2) {
    return false;
  }
  guest_output_width = width;
  guest_output_height = height;
  // 1:1 pixel mapping: the presenter shows the image edge-to-edge on the
  // window it was sized for.
  display_width = width;
  display_height = height;
  return true;
}

bool ShouldSuppressEmulatedDraws() {
  return REXCVAR_GET(native_render_suppress_emulated_draws) &&
         g_native_output_active.load(std::memory_order_relaxed);
}

bool ShouldSuppressExemptDepthOnlyDraws() {
  return REXCVAR_GET(native_render_suppress_exempt_depth_only);
}

namespace {
// Last native frame each 64 KB chunk of physical memory was sampled.
constexpr uint32_t kChunkShift = 16;
constexpr uint32_t kChunkCount = (0x20000000u >> kChunkShift);
std::atomic<uint32_t> g_sampled_frame[kChunkCount];
std::atomic<uint32_t> g_native_sample_frame{0};

struct PassHistory {
  uint32_t dest = 0;
  uint64_t bits = 0;         // bit 0 = the frame `last_frame`, bit k = k frames earlier
  uint64_t last_frame = 0;
};
std::mutex g_pass_mutex;
std::unordered_map<uint32_t, PassHistory> g_pass_history;

uint64_t HistoryAt(const PassHistory& h, uint64_t frame) {
  if (frame < h.last_frame) {
    return h.bits;
  }
  const uint64_t age = frame - h.last_frame;
  return age >= 64 ? 0 : (h.bits << age);
}
}  // namespace

void MarkNativeSampledGuestAddress(uint32_t guest_address) {
  const uint32_t chunk = (guest_address & 0x1FFFFFFFu) >> kChunkShift;
  g_sampled_frame[chunk].store(g_native_sample_frame.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
}

void AdvanceNativeSampleFrame() {
  g_native_sample_frame.fetch_add(1, std::memory_order_relaxed);
}

uint32_t EmulatedPassKey(uint32_t surface_info, uint32_t color_info, uint32_t depth_info) {
  uint32_t h = 2166136261u;
  for (uint32_t v : {surface_info, color_info, depth_info}) {
    h = (h ^ v) * 16777619u;
  }
  return h;
}

void NoteEmulatedResolve(uint32_t pass_key, uint32_t dest_address, uint64_t emulated_frame) {
  std::lock_guard<std::mutex> lock(g_pass_mutex);
  if (g_pass_history.size() > 4096) {
    g_pass_history.clear();  // growth backstop (map changes)
  }
  PassHistory& h = g_pass_history[pass_key];
  const uint32_t dest = dest_address & 0x1FFFFFFFu;
  if (h.dest != dest) {
    h = PassHistory{};  // the setup now resolves elsewhere: relearn
    h.dest = dest;
  }
  h.bits = HistoryAt(h, emulated_frame) | 1u;
  h.last_frame = std::max(h.last_frame, emulated_frame);
}

bool IsUnconsumedPerFramePass(uint32_t pass_key, uint64_t emulated_frame) {
  if (!REXCVAR_GET(native_render_cull_unconsumed_passes) ||
      REXCVAR_GET(native_render_force_resolve_readback_max_length) > 0) {
    return false;
  }
  const uint32_t native_frame = g_native_sample_frame.load(std::memory_order_relaxed);
  if (native_frame < 240) {
    return false;  // let the native renderer establish what it samples
  }
  uint32_t dest;
  uint64_t bits;
  {
    std::lock_guard<std::mutex> lock(g_pass_mutex);
    const auto it = g_pass_history.find(pass_key);
    if (it == g_pass_history.end()) {
      return false;  // unknown pass: execute
    }
    dest = it->second.dest;
    // History as of the PREVIOUS emulated frame (this frame's resolve may
    // not have happened yet).
    bits = HistoryAt(it->second, emulated_frame) >> 1;
  }
  if (std::popcount(bits & 0x7FFFFFFFFFFFFFFFull) < 48) {
    return false;  // not a steady per-frame pass (one-shot / occasional)
  }
  const uint32_t sampled = g_sampled_frame[dest >> kChunkShift].load(std::memory_order_relaxed);
  const bool cull = sampled == 0 || native_frame - sampled > 120;
  if (cull) {
    static std::mutex s_log_mutex;
    static std::unordered_map<uint32_t, uint32_t> s_logged;  // key -> dest
    std::lock_guard<std::mutex> lock(s_log_mutex);
    const auto [it, fresh] = s_logged.emplace(pass_key, dest);
    if ((fresh || it->second != dest) && s_logged.size() < 256) {
      it->second = dest;
      REXLOG_INFO("native pass cull: skipping per-frame emulated pass key={:08X} -> 0x{:08X} "
                  "(output never sampled by the native renderer)",
                  pass_key, dest);
    }
  }
  return cull;
}

bool ShouldSuppressPassAtPitch(uint32_t surface_pitch) {
  switch (REXCVAR_GET(native_render_suppress_mode)) {
    case 0:
      return surface_pitch >= 1280;
    case 1:
      return true;
    case 3:
      // Portrait-window mode: the one-shot frontend portrait RTTs (Skate 3
      // census during the window: 1200/800/640/600/560 + small mips)
      // execute, while the 1152-wide main scene/postfx band, the
      // whole-pipeline cost at scaled resolutions, stays suppressed like
      // the framebuffer.
      return surface_pitch >= 1280 || surface_pitch == 1152;
    default:
      // Execute only the memory-composition passes the native renderer
      // samples: lightmap page composition (1024) and small composite
      // surfaces (<= 512, CAS outfit pieces). The <= 512 window also lets a
      // few tiny postfx pyramid mips through, negligible, and safer than
      // guessing which small surfaces matter.
      return !(surface_pitch == 1024 || surface_pitch <= 512);
  }
}

}  // namespace rex::graphics

namespace rex::graphics::nrhi {
namespace {

// Set once during app startup before any CreateShader (see native_rhi.h);
// lives in this shared TU so every backend reads the same value.
std::string g_shader_bytecode_cache_dir;

}  // namespace

void SetShaderBytecodeCacheDirectory(const char* path) {
  g_shader_bytecode_cache_dir = path != nullptr ? path : "";
}

const char* GetShaderBytecodeCacheDirectory() {
  return g_shader_bytecode_cache_dir.c_str();
}

}  // namespace rex::graphics::nrhi
