#pragma once

// D3D12 implementation of the native-render RHI (rex/graphics/native_rhi.h).
// Constructed and owned by the D3D12 command processor; a thin passthrough
// onto the deferred command list, the CP barrier queue and the CP submission
// counters, reproducing exactly the D3D12 usage the scene renderer was
// originally written against.

#include <cstdint>
#include <functional>

#include <rex/graphics/native_rhi.h>
#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::graphics::d3d12 {

class D3D12CommandProcessor;
class DeferredCommandList;

// Creates the device wrapper (call once; destroy with
// DestroyNativeRhiDevice after all GPU work completed).
nrhi::Device* CreateNativeRhiDevice(D3D12CommandProcessor* command_processor);
void DestroyNativeRhiDevice(nrhi::Device* device);

// Per-frame, inside the guest-output refresher: wraps the presenter's guest
// output resource (cached by identity), drains completed retirement, and
// returns the frame Cmd. guest_output_out receives the wrapped texture.
nrhi::Cmd* NativeRhiBeginFrame(nrhi::Device* device,
                               ID3D12Resource* guest_output_resource,
                               DXGI_FORMAT guest_output_format,
                               D3D12_RESOURCE_STATES guest_output_internal_state,
                               uint32_t width, uint32_t height,
                               nrhi::Texture** guest_output_out);

// Mid-frame offscreen job (native resolve jobs): the frame Cmd without a
// guest output; kGuestOutput maps to PIXEL_SHADER_RESOURCE until the next
// NativeRhiBeginFrame.
nrhi::Cmd* NativeRhiBeginOffscreen(nrhi::Device* device);

// ---- Submission timeline -------------------------------------------------
// The RHI keeps its own fence-backed submission counter (what
// nrhi::Device::CurrentSubmission / CompletedSubmission report). Whoever
// submits the RHI's recorded commands calls NativeRhiSignalSubmitted right
// after on the same queue: the command processor after each of its
// submissions while it records native frames, the native render thread after
// each of its own.
void NativeRhiSignalSubmitted(nrhi::Device* device, ID3D12CommandQueue* queue);
uint64_t NativeRhiCurrentSubmission(nrhi::Device* device);
void NativeRhiAwaitSubmission(nrhi::Device* device, uint64_t submission);

// ---- Recording target ----------------------------------------------------
// nullptr (the default) records into the command processor's deferred
// command list and barrier queue. A non-null list makes the RHI record there
// (barriers inline), for the native render thread; `flush` submits that list
// mid-frame for nrhi::Device::FlushSubmission.
void NativeRhiSetAsyncTarget(nrhi::Device* device, DeferredCommandList* list,
                             std::function<bool()> flush);

}  // namespace rex::graphics::d3d12
