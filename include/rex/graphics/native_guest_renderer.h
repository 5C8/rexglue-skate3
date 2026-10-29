#pragma once

#include <cstdint>

#include <rex/graphics/native_rhi.h>

namespace rex::graphics {

enum class NativeGuestOutputBackend : uint32_t {
  kUnknown = 0,
  kD3D12,
  kVulkan,
};

// Per-frame context handed to the registered native guest-output renderer.
// Backend-agnostic: all device access and command recording goes through the
// native-render RHI (rex/graphics/native_rhi.h); the command processor owns
// the nrhi::Device and the per-frame nrhi::Cmd.
struct NativeGuestOutputRenderContext {
  NativeGuestOutputBackend backend = NativeGuestOutputBackend::kUnknown;
  uint32_t guest_output_width = 0;
  uint32_t guest_output_height = 0;
  uint32_t display_width = 0;
  uint32_t display_height = 0;

  // Device-level RHI: resource/pipeline creation, submission counters.
  // Stable across frames for the lifetime of the graphics system.
  nrhi::Device* device = nullptr;
  // Frame-scoped command recording into the command processor's deferred
  // command list. Only valid during the callback.
  nrhi::Cmd* cmd = nullptr;
  // The presenter's guest output image, wrapped as an RHI texture. In
  // nrhi::ResourceState::kGuestOutput at entry (or kCommon the very first
  // frame it exists); must be returned to kGuestOutput before the callback
  // returns true. The pointer is stable while the underlying image is (it
  // changes on output resize).
  nrhi::Texture* guest_output = nullptr;
};

using NativeGuestOutputRenderer = bool (*)(const NativeGuestOutputRenderContext& context,
                                           void* user_data);

void SetNativeGuestOutputRenderer(NativeGuestOutputRenderer renderer, void* user_data);
bool TryRenderNativeGuestOutput(const NativeGuestOutputRenderContext& context);
// Whether a renderer is registered at all; command processors skip RHI
// setup entirely when none is (non-Skate titles / renderer disabled).
bool HasNativeGuestOutputRenderer();

// True while the registered renderer actually replaced the last presented
// frame (false when it yields to the emulated output).
bool IsNativeGuestOutputActive();

// ---- Inter-frames -----------------------------------------------------------
// Extra native frames presented BETWEEN guest swaps. The guest's render
// thread bounds the swap rate, while the command processor thread and the GPU
// usually have headroom: whenever the command processor runs out of guest
// commands it asks `want`, and on true presents one more frame through the
// registered renderer, sized like the last native swap. IsNativeInterFrame()
// is true for the duration of that render (the renderer re-renders its last
// scene advanced to the present time). An inter-frame render that returns
// false keeps the previously presented image and does not change
// IsNativeGuestOutputActive(). Command processor thread.
struct NativeInterFrameHooks {
  bool (*want)(void* user_data) = nullptr;
  // Optional: may native frames (swap frames and inter-frames) be rendered
  // on the command processor's native render thread instead of the command
  // processor thread itself? Asked once per guest swap; false (or no hook)
  // keeps everything on the command processor. The renderer is only ever
  // called on one of the two threads at a time.
  bool (*allow_async)(void* user_data) = nullptr;
  // Optional: seconds until `want` will next return true (<= 0: now), or a
  // negative value below -0.5 when no inter-frame is due before the next
  // guest swap. Lets the native render thread sleep precisely instead of
  // polling `want`.
  double (*seconds_until_want)(void* user_data) = nullptr;
  // Optional (native render thread only): steady_clock time, in seconds
  // since its epoch, the frame just rendered should be handed to the
  // presenter at (<= 0: at once). Asked after the frame's GPU work has been
  // submitted; the thread holds the finished frame until then, so frames
  // whose content is posed for a planned display time are shown at that
  // time regardless of how long their CPU recording happened to take.
  double (*present_at)(void* user_data) = nullptr;
  void* user_data = nullptr;
};
void SetNativeInterFrameHooks(const NativeInterFrameHooks& hooks);
// nullptr when none are registered.
const NativeInterFrameHooks* GetNativeInterFrameHooks();
bool IsNativeInterFrame();
// Renders an inter-frame through the registered renderer (the command
// processors' TryRenderNativeGuestOutput counterpart): sets
// IsNativeInterFrame() around the call and leaves the active flag alone.
bool TryRenderNativeInterFrame(const NativeGuestOutputRenderContext& context);
// Real (guest) frame rate next to the presented one: the command processors
// note every guest swap and every presented inter-frame; the FPS overlay
// shows "N FPS (M real)" while inter-frames were presented within the last
// second. Thread-safe.
void NoteNativeGuestSwap();
void NoteNativeInterFramePresented();
// Guest swaps per second over the last second (0 when none), and whether an
// inter-frame was presented within the last second.
double NativeGuestSwapRate();
bool NativeInterFramesRecent();

// ---- Post-submit hook --------------------------------------------------------
// Invoked on the command processor thread right after a guest-output refresh
// has been submitted to the GPU queue (native frames, and emulated frames
// that ran the post-processor), i.e. once everything recorded through that
// frame's nrhi::Cmd is on the queue. External consumers that read the
// frame's textures on the same queue (the OpenVR compositor's Submit) belong
// here: anything they enqueue is ordered after the frame's rendering.
using NativeGuestOutputSubmitHook = void (*)(NativeGuestOutputBackend backend, void* user_data);
void SetNativeGuestOutputSubmitHook(NativeGuestOutputSubmitHook hook, void* user_data);
void InvokeNativeGuestOutputSubmitHook(NativeGuestOutputBackend backend);

// ---- Wide guest output ----------------------------------------------------
// Display aspect the native renderer wants to draw at when it is wider than
// the guest frontbuffer's own aspect (ultrawide). 0 = disabled. Set once at
// app startup; consumed by the command processors when sizing the guest
// output for a swap.
void SetNativeGuestOutputWideAspect(double aspect);
double GetNativeGuestOutputWideAspect();
// Applies the wide aspect to this swap's guest output and display
// dimensions: widens guest_output_width to guest_output_height * aspect and
// makes the display aspect match. Only widens while a renderer is registered
// AND it served the last presented frame, so emulated fallback frames size
// back to the frontbuffer aspect and present pillarboxed by the presenter's
// letterbox path. Returns whether the output was widened; when it was and
// the renderer then yields the frame, the refresh callback must return false
// (keeping the previously presented image) instead of running the emulated
// blit, which writes frontbuffer-aspect content.
bool ApplyNativeGuestOutputWideAspect(uint32_t& guest_output_width, uint32_t guest_output_height,
                                      uint32_t& display_width, uint32_t& display_height);

// ---- Native guest output size ---------------------------------------------
// Optional provider for the exact pixel size the native renderer draws at
// (e.g. the window's physical client size, so the output maps 1:1 onto the
// screen instead of frontbuffer x draw_resolution_scale). Returning false
// (or a degenerate size) keeps the default sizing. Called on the GPU thread
// once per swap. Like the wide aspect, it only applies while a renderer is
// registered AND served the last presented frame; emulated fallback frames
// keep the frontbuffer-derived size.
using NativeGuestOutputSizeProvider = bool (*)(uint32_t& width, uint32_t& height,
                                               void* user_data);
void SetNativeGuestOutputSizeProvider(NativeGuestOutputSizeProvider provider, void* user_data);
// Applies the provider's size (rounded down to even, clamped to 16384) to this
// swap's guest output and display dimensions. Returns whether it applied;
// same contract as ApplyNativeGuestOutputWideAspect when the renderer then
// yields the frame (keep the previously presented image).
bool ApplyNativeGuestOutputSize(uint32_t& guest_output_width, uint32_t& guest_output_height,
                                uint32_t& display_width, uint32_t& display_height);
// native_render_suppress_emulated_draws && IsNativeGuestOutputActive():
// command processors skip emulated draw/resolve execution (memexport draws,
// fences, queries and PM4 parsing still run).
bool ShouldSuppressEmulatedDraws();

// While the native guest-output renderer is active: should the emulated pass
// currently targeting `surface_pitch`-wide surfaces be suppressed? Driven by
// the native_render_suppress_mode cvar; shared by both command processors.
// Draw and resolve suppression must agree: executed passes need their
// resolves, suppressed passes leave garbage EDRAM that must never be copied
// out.
bool ShouldSuppressPassAtPitch(uint32_t surface_pitch);

// ---- Consumer-driven pass culling ------------------------------------------
// An emulated pass is worth executing only if something reads its output.
// The native renderer publishes which guest texture addresses it sampled
// (MarkNativeSampledGuestAddress, per native frame); the command processor
// records, per render-target setup (pass key), where that pass resolves
// (NoteEmulatedResolve). A pass that resolves into the same destination on
// nearly every emulated frame while the native renderer has not sampled that
// destination for a while is a per-frame pass nobody reads (the game's own
// shadow blur/convert, world-shade map, bloom mips): its draws and resolves
// are skipped. One-shot passes (lightmap page composition at stream-in,
// portraits) are never "per-frame" and always execute; per-frame passes the
// native renderer samples (live CAS composites) execute. Everything executes
// while the forced-readback (photo) window is armed.
void MarkNativeSampledGuestAddress(uint32_t guest_address);
void AdvanceNativeSampleFrame();
uint32_t EmulatedPassKey(uint32_t surface_info, uint32_t color_info, uint32_t depth_info);
void NoteEmulatedResolve(uint32_t pass_key, uint32_t dest_address, uint64_t emulated_frame);
bool IsUnconsumedPerFramePass(uint32_t pass_key, uint64_t emulated_frame);

// Within a suppression-EXEMPT pass: should a draw with no pixel shader
// (depth/stencil-only: shadow casters, z-prepasses) be skipped anyway?
// Driven by native_render_suppress_exempt_depth_only; their output feeds
// only suppressed scene passes (the native renderer shadows itself).
bool ShouldSuppressExemptDepthOnlyDraws();

// ---- Guest-output post-processor ------------------------------------------
// Host effect over the EMULATED guest-output path (e.g. the settings-menu
// backdrop blur): invoked by the command processors at the very end of the
// emulated gamma/FXAA refresh, after the guest output image has been fully
// written, with the same context contract as the renderer callback (the
// image arrives in kGuestOutput state and must be returned to it). When the
// native renderer handled the frame this is NOT invoked; the renderer
// applies its own effects inline. Only invoked while the request flag is set
// (a cheap gate so the common no-effect frame skips the RHI frame setup);
// the post-processor clears the flag itself once its effect has fully
// decayed.
using NativeGuestOutputPostProcessor = void (*)(const NativeGuestOutputRenderContext& context,
                                                void* user_data);
// ---- Native resolve jobs ---------------------------------------------------
// While emulated draws are suppressed, a guest resolve (EDRAM -> memory copy)
// whose source pass the native renderer reproduces can be claimed by it: the
// command processor then skips the emulated resolve, hands the renderer a
// frame-scoped Cmd (no guest output; guest_output = nullptr, and the RHI's
// kGuestOutput state maps to PIXEL_SHADER_RESOURCE for the job), submits,
// waits for the GPU and calls complete() so the renderer can copy its result
// into guest memory before the guest continues - the same synchronization
// the emulated readback-resolve path gives a guest that CPU-reads the
// resolved texture. Called on the command processor thread.
struct NativeResolveJobHooks {
  // A guest resolve to dest_address (physical) is about to execute.
  bool (*claim)(uint32_t dest_address, void* user_data) = nullptr;
  // Record the claimed job's GPU work.
  bool (*render)(const NativeGuestOutputRenderContext& context, void* user_data) = nullptr;
  // The GPU work recorded by render() completed (rendered = its result).
  void (*complete)(bool rendered, void* user_data) = nullptr;
  void* user_data = nullptr;
};
void SetNativeResolveJobHooks(const NativeResolveJobHooks& hooks);
// nullptr when none are registered.
const NativeResolveJobHooks* GetNativeResolveJobHooks();

void SetNativeGuestOutputPostProcessor(NativeGuestOutputPostProcessor post_processor,
                                       void* user_data);
bool HasNativeGuestOutputPostProcessor();
void InvokeNativeGuestOutputPostProcessor(const NativeGuestOutputRenderContext& context);
void RequestNativeGuestOutputPostProcess(bool requested);
bool IsNativeGuestOutputPostProcessRequested();

}  // namespace rex::graphics
