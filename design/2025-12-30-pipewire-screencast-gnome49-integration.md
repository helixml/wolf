# PipeWire ScreenCast CUDA Integration for GNOME 49+ (Technical Design)

**Date:** 2025-12-30
**Author:** Luke / Claude
**Status:** Implementation compiles, pending context integration and testing

## Executive Summary

GNOME 49 removes wl-roots-style Wayland capture. This document analyzes whether we can use the proven gst-wayland-display EGL→CUDA code to convert PipeWire ScreenCast DMA-BUFs into CUDA buffers.

**Conclusion:** Yes, technically feasible. The gst-wayland-display code converts **DMA-BUFs** to CUDA buffers (not EGL surfaces), and PipeWire ScreenCast outputs DMA-BUFs. The same conversion path applies.

---

## The Real Problem: CUDA Buffer Sharing in Lobby Mode

The Wolf maintainer's concern about `pipewiresrc ! glupload ! cudaupload` being "fragile" relates to **CUDA buffer sharing between multiple viewers in lobby mode**.

When multiple Moonlight clients connect to the same Wolf session:
- The video source outputs frames once
- Multiple encoders consume the same frames
- Sharing CUDA buffers between consumers requires careful synchronization
- The `glupload ! cudaupload` pipeline adds GL context complexity to this already complex sharing

The gst-wayland-display approach outputs CUDA buffers **directly**, avoiding the GL intermediary and its associated context/synchronization complexity.

---

## Technical Deep Dive: What Does gst-wayland-display Actually Do?

### Input: DMA-BUF (NOT EGL Surface)

The CUDA conversion code in `wayland-display-core/src/utils/allocator/cuda/` takes:
- A `Dmabuf` (smithay type) containing file descriptors to kernel DMA buffers
- An `EGLDisplay` for the GPU

**NOT** an EGL surface or GL texture. The EGL is just an intermediary for CUDA interop.

### The Conversion Pipeline

```c
// Step 1: Create EGLImage from DMA-BUF
// File: wayland-display-core/src/utils/allocator/cuda/mod.rs:55-134
attribs = [
    EGL_LINUX_DRM_FOURCC_EXT, fourcc,
    EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf.fd,
    EGL_DMA_BUF_PLANE0_OFFSET_EXT, dmabuf.offset,
    EGL_DMA_BUF_PLANE0_PITCH_EXT, dmabuf.stride,
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, modifier_lo,
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, modifier_hi,
    ...
];
egl_image = eglCreateImageKHR(
    egl_display,
    EGL_NO_CONTEXT,        // No GL context needed!
    EGL_LINUX_DMA_BUF_EXT, // Target: DMA-BUF import
    NULL,
    attribs
);

// Step 2: Register EGLImage with CUDA
// File: wayland-display-core/src/utils/allocator/cuda/mod.rs:448-463
cuGraphicsEGLRegisterImage(&cuda_resource, egl_image, 0);

// Step 3: Get CUDA-accessible frame data
// File: wayland-display-core/src/utils/allocator/cuda/ffi.rs:208-221
cuGraphicsResourceGetMappedEglFrame(&egl_frame, cuda_resource, 0, 0);

// Step 4: Copy to CUDA buffer
// File: wayland-display-core/src/utils/allocator/cuda/ffi.rs:463-538
CuMemcpy2DAsync(&copy_params, stream);
```

### Key Observation

The conversion is **generic for any DMA-BUF on the same GPU**. It doesn't care whether the DMA-BUF came from:
- A Wayland client rendering to Wolf's compositor (current use case)
- GNOME Shell's ScreenCast via PipeWire (proposed use case)
- Any other source that produces DMA-BUFs

### Important: Context Sharing (Wolf Maintainer Feedback)

> **Note:** "There are more nuances to that, for example we reuse the EGL context from Smithay and externally make and share the CUDA context in Wolf in order to properly pass that CUDA buffer from one plugin to the next."

The implementation must:
1. **Reuse Wolf's existing EGL context** (from Smithay) rather than creating a new one
2. **Share the CUDA context** with other Wolf components for buffer passing between plugins
3. **Not create isolated contexts** that would break inter-plugin buffer sharing

This means the gst-pipewire-zerocopy element needs to receive the EGL/CUDA context from Wolf rather than creating its own. The `waylanddisplaycore` approach already supports this via context injection.

---

## What Does PipeWire ScreenCast Output?

### Buffer Types

PipeWire ScreenCast outputs **DMA-BUFs** when negotiated. From [PipeWire DMA-BUF documentation](https://docs.pipewire.org/page_dma_buf.html):

1. **DMA-BUF mode** (preferred): `SPA_DATA_DmaBuf`
   - File descriptors to GPU memory
   - Includes fourcc format code and modifiers
   - Same structure as what gst-wayland-display handles

2. **SHM fallback**: `SPA_DATA_MemFd` / `SPA_DATA_MemPtr`
   - CPU memory (used when DMA-BUF negotiation fails)
   - Requires CPU→GPU upload

### GNOME/Mutter Implementation

From [Mutter MR !1939](https://gitlab.gnome.org/GNOME/mutter/-/merge_requests/1939) and [MR !2086](https://gitlab.gnome.org/GNOME/mutter/-/merge_requests/2086):

- Mutter announces both DMA-BUF and SHM capabilities via PipeWire
- DMA-BUF buffers are only allocated if the PipeWire client requests them
- On NVIDIA, Mutter uses the same GPU memory for ScreenCast as for rendering

### Format Negotiation

PipeWire uses SPA (Simple Plugin API) for format negotiation:
```
Consumer → Announces: "I accept DMA-BUF with modifiers X, Y, Z"
Producer → Responds with: Best matching format/modifier
```

The consumer (our element) must:
1. Query supported modifiers from our EGL/CUDA stack
2. Announce them to PipeWire
3. Accept buffers with negotiated format

---

## Why the Current `pipewiresrc ! glupload ! cudaupload` is Problematic

### The Pipeline
```
pipewiresrc (outputs DMA-BUF)
    ↓
glupload (imports DMA-BUF into GL texture)
    ↓
cudaupload (copies GL texture to CUDA)
    ↓
nvh264enc
```

### Problems

1. **GL Context Complexity**: `glupload` creates/manages GL context, which adds state that must be coordinated with CUDA context.

2. **Buffer Sharing in Lobby Mode**: When multiple encoders share frames:
   - GL textures have their own reference counting
   - GL→CUDA synchronization is per-texture
   - Multiple consumers = multiple sync points = instability

3. **Extra Copy**: `glupload` may perform format conversion into GL texture, then `cudaupload` copies again to CUDA. Two copies vs our one.

4. **Error Propagation**: Failures in `glupload` (EGL context errors, format mismatches) are opaque to the pipeline.

### Our Approach: Direct DMA-BUF → CUDA

```
pipewiresrc (outputs DMA-BUF)
    ↓ [OUR ELEMENT]
pipewire-cuda-src (DMA-BUF → EGLImage → CUDA, single copy)
    ↓
nvh264enc
```

Benefits:
- Single CUDA context, no GL context needed
- One copy operation (EGL frame → CUDA buffer)
- Direct control over buffer lifecycle
- Cleaner error handling
- Proven code path from gst-wayland-display

---

## Code Reuse Analysis

### What We Reuse from gst-wayland-display

| Module | Path | Reusable? | Notes |
|--------|------|-----------|-------|
| CUDA FFI | `utils/allocator/cuda/ffi.rs` | Yes | All CUDA interop functions |
| EGLImage creation | `utils/allocator/cuda/mod.rs:EGLImage` | Yes | DMA-BUF → EGLImage |
| CUDAImage | `utils/allocator/cuda/mod.rs:CUDAImage` | Yes | EGLImage → CUDA |
| GStreamer buffer pool | `utils/allocator/cuda/mod.rs:CUDABufferPool` | Yes | CUDA memory management |
| wl-roots compositor | `wayland/*`, `comp/*` | No | Not needed for PipeWire |
| GStreamer PushSrc pattern | `gst-plugin-wayland-display/waylandsrc/imp.rs` | Partial | Different buffer source |

### What We Build New

| Component | Description |
|-----------|-------------|
| PipeWire ScreenCast client | Connect to portal, negotiate DMA-BUF format |
| DMA-BUF adapter | Convert PipeWire's SPA buffer to smithay's `Dmabuf` |
| GStreamer element | PushSrc that outputs CUDA buffers |

---

## Proposed Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                    gst-pipewire-zerocopy                            │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────────────┐  │
│  │  PipeWire    │    │   DMA-BUF    │    │   CUDA Converter     │  │
│  │  Client      │───▶│   Adapter    │───▶│   (from gst-wayland- │  │
│  │  (ashpd)     │    │              │    │    display)          │  │
│  └──────────────┘    └──────────────┘    └──────────────────────┘  │
│         │                                          │                │
│         │ SPA buffers                              │ CUDA buffers   │
│         │ (DMA-BUF FDs)                            ▼                │
│         │                              ┌──────────────────────┐     │
│         └─────────────────────────────▶│  GStreamer PushSrc   │     │
│                                        │  Element             │     │
│                                        └──────────────────────┘     │
└─────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
                          video/x-raw(memory:CUDAMemory)
```

### GPU Support Matrix

| GPU | Buffer Type | Implementation |
|-----|-------------|----------------|
| NVIDIA | `video/x-raw(memory:CUDAMemory)` | EGLImage → CUDA (reuse gst-wayland-display) |
| AMD/Intel | `video/x-raw(memory:DMABuf)` | Pass through DMA-BUF directly |
| Software | `video/x-raw` | SHM fallback, CPU copy |

---

## Validation: Will This Actually Work?

### Technical Requirements

1. **Same GPU**: PipeWire DMA-BUFs must be from the same GPU as CUDA context
   - **Wolf controls this**: Container runs on specific GPU, GNOME renders there

2. **EGL_EXT_image_dma_buf_import**: Must be supported
   - **NVIDIA supports this**: Since driver 470+

3. **cuGraphicsEGLRegisterImage**: Must work on imported EGLImages
   - **Already works**: This is exactly what gst-wayland-display uses

4. **Modifier compatibility**: GNOME's modifiers must be importable
   - **Negotiable**: We announce our supported modifiers to PipeWire

### Risk Assessment

| Risk | Likelihood | Mitigation |
|------|------------|------------|
| GNOME uses unsupported modifier | Low | Negotiate LINEAR modifier as fallback |
| Buffer lifecycle mismatch | Medium | Use pw_stream buffer callbacks properly |
| CUDA context issues | Low | Reuse gst-wayland-display's proven context management |
| Format conversion needed | Low | Negotiate compatible formats upfront |

### Proof of Concept Test

Before full implementation, validate with:
```bash
# In Wolf container with GNOME 49
# 1. Start GNOME ScreenCast session
# 2. Manually import DMA-BUF into EGLImage
# 3. Register with CUDA
# 4. Verify cuGraphicsResourceGetMappedEglFrame succeeds
```

---

## Implementation Plan

### Phase 1: Validation (1-2 days)
1. Build minimal PipeWire client that receives DMA-BUFs
2. Import received DMA-BUF using gst-wayland-display's `EGLImage::from`
3. Convert to CUDA using `CUDAImage::from`
4. Verify data integrity

### Phase 2: GStreamer Element (3-5 days)
1. Create `gst-pipewire-zerocopy` crate in Wolf repo
2. Add wayland-display-core as git dependency
3. Implement PushSrc element
4. Handle CUDA/DMABuf/SHM output modes

### Phase 3: Integration (2-3 days)
1. Update Wolf Dockerfile to build new plugin
2. Add `video_source_mode: pipewire` configuration
3. Test with GNOME 49 container
4. Test lobby mode with multiple viewers

---

## Input Handling

### Wolf's Current Approach: Direct Wayland Input Injection

> **Note (Wolf maintainer feedback):** Wolf does **NOT** use uinput virtual devices. It directly injects mouse, keyboard, and touch events into Wayland without any virtual device involvement.

```
┌─────────────────────────────────────────────────────────────────┐
│  Wolf (Host)                                                     │
│  └── Direct Wayland protocol input injection                    │
│      ├── wl_pointer events → Wayland compositor                 │
│      ├── wl_keyboard events → Wayland compositor                │
│      ├── wl_touch events → Wayland compositor                   │
│      └── Gamepad → Direct to compositor or libinput             │
└─────────────────────────────────────────────────────────────────┘
```

**Advantages:**
- No kernel access or CAP_SYS_ADMIN required
- No issues with uinput device creation/permissions
- Low latency (direct Wayland protocol)
- Works with Wolf's embedded Sway compositor

### For PipeWire ScreenCast Mode

When using GNOME Shell as the desktop (not Wolf's embedded Sway), input injection approach depends on what the desktop supports:

1. **If Wolf controls the Wayland compositor**: Continue using direct Wayland input injection
2. **If connecting to an external GNOME session**: May need RemoteDesktop Portal for input

**No changes needed for initial implementation** - the PipeWire ScreenCast integration only affects video capture. Input handling is separate and can be addressed based on the specific deployment scenario.

---

## Questions for Wolf Maintainer

1. **Buffer pool sharing**: Should we reuse GstCudaBufferPool pattern from gst-wayland-display for consistent buffer management?

2. **Modifier preferences**: Any specific modifiers to prefer/avoid for encoder compatibility?

3. **Fallback behavior**: If DMA-BUF negotiation fails, fall back to SHM (CPU path) or error out?

4. **Integration point**: New GStreamer element, or modify existing `start_pipewire_video_producer`?

---

## Current Implementation Status

### Files Created

```
wolf/gst-pipewire-zerocopy/
├── Cargo.toml              # Dependencies: gstreamer, pipewire, waylanddisplaycore, smithay
├── build.rs                # GStreamer plugin version helper
└── src/
    ├── lib.rs              # Plugin registration (minimal)
    ├── pipewire_stream.rs  # PipeWire stream → smithay Dmabuf
    └── pipewiresrc/
        ├── mod.rs          # Element wrapper
        └── imp.rs          # PushSrc using waylanddisplaycore's CUDA code
```

**Key Design Decision: Maximum Code Reuse**

The implementation directly reuses waylanddisplaycore's battle-tested types:
- `EGLImage::from(dmabuf, egl_display)` - DMA-BUF → EGLImage
- `CUDAImage::from(egl_image, cuda_context)` - EGLImage → CUDA
- `CUDAImage::to_gst_buffer(video_info, cuda_ctx, buffer_pool)` - CUDA → GStreamer buffer
- `CUDAContext`, `CUDABufferPool` - Context/pool management
- smithay's `Dmabuf` type directly from PipeWire buffers

No custom CUDA FFI code - everything flows through proven gst-wayland-display code.

### Context Sharing Pattern (Wolf Maintainer Feedback)

The element now follows the same context sharing pattern as waylandsrc:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    CUDA Context Sharing Flow                            │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  1. Wolf creates CUDA context                                           │
│     └── gst_cuda_context_new(device_id)                                │
│                                                                         │
│  2. Wolf pushes context to pipeline elements                            │
│     └── gst_element_set_context(element, context)                      │
│                                                                         │
│  3. pipewirezerocopysrc receives in set_context()                      │
│     └── CUDAContext::new_from_set_context() extracts and stores it     │
│                                                                         │
│  4. Downstream elements query for context                               │
│     └── pipewirezerocopysrc responds via query() override              │
│                                                                         │
│  FALLBACK: If Wolf doesn't push context, element acquires via          │
│            CUDAContext::new_from_gstreamer() in start()                │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

Key implementation details:
- `cuda_raw_ptr: AtomicPtr<GstCudaContext>` - raw pointer for GStreamer CUDA interop
- `cuda_context: Arc<Mutex<CUDAContext>>` - shared context accessible across threads
- Both stored in Settings struct for access from set_context(), start(), and create()

### What's Implemented

- ✅ GStreamer PushSrc element (`pipewirezerocopysrc`)
- ✅ Properties: `pipewire-node-id`, `render-node`, `output-mode`, `cuda-device-id`
- ✅ Pad templates for CUDA, DMABuf, and system memory output
- ✅ PipeWire → smithay Dmabuf conversion
- ✅ waylanddisplaycore CUDA path: `EGLImage::from()` → `CUDAImage::from()` → `to_gst_buffer()`
- ✅ SHM fallback for non-DMA-BUF buffers
- ✅ Integration into Wolf Dockerfile build
- ✅ Integration into `streaming.cpp`
- ✅ **Context sharing** (implemented 2025-12-30):
  - `set_context()` override receives CUDA context pushed by Wolf via `gst_element_set_context()`
  - Uses `CUDAContext::new_from_set_context()` to receive context from Wolf
  - Uses `CUDAContext::new_from_gstreamer()` as fallback if no context pushed
  - `query()` override responds to context queries from downstream elements
  - `cuda_raw_ptr: AtomicPtr<GstCudaContext>` for GStreamer CUDA interop
  - Follows exact pattern from waylandsrc in gst-wayland-display

### What's Still TODO

- ❌ Format negotiation with PipeWire (currently accepts any format)
- ❌ End-to-end testing with GNOME 49 container
- ❌ Verify context sharing works correctly when Wolf pushes context

### Build Status

✅ **Compiles successfully** (as of 2025-12-30) - integrated into Wolf Docker build

### Build Requirements

The crate requires PipeWire development libraries:

```bash
# Ubuntu/Debian
apt install libpipewire-0.3-dev

# Inside Wolf Docker container (installed by Dockerfile)
# The crate builds as part of the Wolf build process via cargo cinstall
```

### Pipeline Usage

The new element is used in `streaming.cpp`:

```cpp
// Old fragile pipeline:
// pipewiresrc path={node_id} ! cudaupload ! video/x-raw(memory:CUDAMemory)

// New unified pipeline:
pipewirezerocopysrc pipewire-node-id={node_id} render-node={render_node} output-mode={mode} ! ...
```

The element auto-detects GPU type and outputs the appropriate format:
- NVIDIA: `video/x-raw(memory:CUDAMemory)` via EGL→CUDA zero-copy
- AMD/Intel: `video/x-raw(memory:DMABuf)` passthrough
- Fallback: `video/x-raw` in system memory

---

## References

- [PipeWire DMA-BUF Sharing](https://docs.pipewire.org/page_dma_buf.html)
- [Mutter DMA-BUF ScreenCast MR](https://gitlab.gnome.org/GNOME/mutter/-/merge_requests/1939)
- [gst-wayland-display source](https://github.com/games-on-whales/gst-wayland-display)
- [CUDA-EGL Interop](https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__EGL.html)
- [ashpd ScreenCast](https://docs.rs/ashpd/latest/ashpd/desktop/screencast/) - Rust portal bindings
- [pipewire-rs](https://pipewire.pages.freedesktop.org/pipewire-rs/) - Rust PipeWire bindings
