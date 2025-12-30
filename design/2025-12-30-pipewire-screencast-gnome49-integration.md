# PipeWire ScreenCast Integration for GNOME 49+ (Design Doc)

**Date:** 2025-12-30
**Author:** Luke / Claude
**Status:** DRAFT - Needs Wolf maintainer review
**Wolf Issue:** GNOME 49 removes wl-roots-style direct Wayland capture

## Key Constraint

**Wolf controls the entire GPU stack.** We force everything (GNOME, apps, encoding) to run on a single GPU. Hybrid/multi-GPU is not a supported configuration.

This means:
- DMA-BUFs from GNOME are NVIDIA-native (same device)
- EGL import into CUDA will work (same device, same driver)
- Zero-copy is achievable

## Problem Statement

GNOME 49 removes the ability for external processes to create wl-roots-style Wayland compositors that receive application buffers directly. Wolf's current `waylanddisplaysrc` element (from gst-wayland-display) relies on this capability.

Going forward, GNOME 49+ requires using **PipeWire ScreenCast portal** for screen capture.

## Wolf Maintainer's Concerns (Quoted)

> That's my concern too, DMA buffers and Nvidia are generally quite painful but I haven't looked into how pipewire does it. If `pipewiresrc` can only output DMA (and not CUDABuffers like we do) you'll have to do:
> ```
> pipewiresrc ! glupload ! cudaupload !
> ```
> which kinda works until it doesn't.. Plus there's the issue of mouse/keyboard inputs, unless that's something that pipewiresrc supports?

This doc addresses each concern directly.

---

## Concern 1: DMA Buffers + NVIDIA = Pain

### The Problem

`pipewiresrc` outputs `video/x-raw(memory:DMABuf)`. Wolf needs `video/x-raw(memory:CUDAMemory)` for NVENC.

The naive approach is:
```
pipewiresrc ! glupload ! cudaupload ! nvh264enc
```

But this has known failure modes:
1. **Hybrid graphics**: DMA-BUF from Intel iGPU can't be imported into NVIDIA EGL
2. **Format mismatches**: PipeWire may negotiate formats NVIDIA can't handle
3. **Modifier incompatibility**: Tiled/compressed buffers may fail EGL import

### Why gst-wayland-display Works (and PipeWire is Different)

| Aspect | gst-wayland-display | PipeWire ScreenCast |
|--------|---------------------|---------------------|
| Who creates buffers? | Applications render to Wolf | GNOME Shell creates buffers |
| Who controls GPU? | Wolf (can force NVIDIA) | GNOME (user's choice) |
| Buffer lifecycle | Wolf controls release | PipeWire demands quick return |
| Cross-device? | Never (same compositor) | Often (hybrid graphics) |

**gst-wayland-display's EGL→CUDA code cannot be directly reused** for PipeWire because:
- It assumes buffers originate from the same GPU
- It assumes Wolf controls buffer lifecycle
- It assumes wl-roots compositor semantics

### Proposed Solution: GPU-Aware Pipeline Selection

```
┌─────────────────────────────────────────────────────────────────┐
│                    Pipeline Selection Logic                      │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  1. Query PipeWire buffer device (DMA-BUF ioctl)                │
│  2. Query NVIDIA device ID                                       │
│  3. Compare:                                                     │
│                                                                  │
│     IF same_device:                                              │
│        pipewiresrc ! glupload ! cudaupload ! nvh264enc          │
│        (fast path - same GPU, GL handles import)                 │
│                                                                  │
│     ELSE IF nvidia_present_but_not_source:                      │
│        pipewiresrc ! gldownload ! cudaupload ! nvh264enc        │
│        (slow path - CPU copy, but guaranteed to work)            │
│                                                                  │
│     ELSE (no nvidia, AMD/Intel only):                           │
│        pipewiresrc ! vapostproc ! vaapih264enc                  │
│        (native VAAPI path - DMABuf stays on GPU)                 │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Code Reuse from gst-wayland-display

We CAN reuse:
- `wayland-display-core/src/utils/allocator/cuda/` - CUDA FFI bindings and CuGraphicsResource wrapper
- `wayland-display-core/src/utils/device.rs` - GPU device detection
- Error handling patterns and logging

We CANNOT reuse:
- The wl-roots compositor code (irrelevant for PipeWire)
- The EGLImage import path directly (different buffer ownership model)
- Buffer lifecycle management (PipeWire has its own)

### Honest Assessment

The `glupload ! cudaupload` pipeline is actually **more robust** than a custom zero-copy element because:
1. GStreamer's `glupload` handles cross-device cases via fallback
2. `cudaupload` is maintained by GStreamer devs who understand NVIDIA quirks
3. It's been battle-tested across many configurations

**Recommendation**: Use the standard GStreamer elements with GPU detection fallback. Don't build a custom zero-copy element that will fail on hybrid graphics.

---

## Concern 2: Mouse/Keyboard Input via PipeWire?

### The Problem

Wolf currently handles input via:
1. Moonlight client sends input events
2. Wolf injects into the Wayland compositor (libinput/uinput)
3. Applications receive native Wayland input

With PipeWire ScreenCast:
- It's **capture only** - no input injection
- The RemoteDesktop portal exists but is separate

### PipeWire RemoteDesktop Portal

GNOME provides `org.freedesktop.portal.RemoteDesktop`:
- Separate from ScreenCast portal
- Provides input injection (keyboard, mouse, touch)
- Must be used together with ScreenCast for full remote desktop

```
┌─────────────────────────────────────────────────────────────────┐
│                 Full Remote Desktop Stack                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────────┐     ┌──────────────────┐                  │
│  │ ScreenCast Portal│     │RemoteDesktop     │                  │
│  │ (capture)        │     │Portal (input)    │                  │
│  └────────┬─────────┘     └────────┬─────────┘                  │
│           │                        │                             │
│           └──────────┬─────────────┘                             │
│                      │                                           │
│               Session (linked)                                   │
│                      │                                           │
│           ┌──────────┴──────────┐                               │
│           │                     │                                │
│      PipeWire stream      D-Bus input                           │
│      (video frames)       injection                              │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### API for Input Injection

```python
# Simplified - actual implementation is D-Bus
session = portal.CreateSession()
portal.SelectDevices(session, {
    'types': KEYBOARD | POINTER,
    'persist_mode': PERSIST_MODE_PERSISTENT
})
portal.Start(session)

# Input injection via D-Bus
portal.NotifyPointerMotion(session, dx, dy)
portal.NotifyPointerButton(session, button, state)
portal.NotifyKeyboardKeycode(session, keycode, state)
```

### Integration with Wolf

Wolf would need:
1. **Portal session management**: Create linked ScreenCast + RemoteDesktop session
2. **Input translation**: Convert Moonlight input events to portal calls
3. **Latency consideration**: D-Bus adds latency vs. direct uinput

### Rust Crates Available

- `ashpd` - Rust bindings for XDG portals (ScreenCast + RemoteDesktop)
- `zbus` - D-Bus library (used by ashpd internally)

```rust
use ashpd::desktop::remote_desktop::{RemoteDesktop, DeviceType};
use ashpd::desktop::screencast::{ScreenCast, SourceType};

async fn create_session() -> Result<Session> {
    let portal = RemoteDesktop::new().await?;
    let session = portal.create_session().await?;

    portal.select_devices(
        &session,
        DeviceType::Keyboard | DeviceType::Pointer,
    ).await?;

    // Link with ScreenCast
    let screencast = ScreenCast::new().await?;
    screencast.select_sources(
        &session,
        SourceType::Monitor,
    ).await?;

    portal.start(&session).await?;
    Ok(session)
}
```

### Honest Assessment

Input via RemoteDesktop portal:
- **Works** - GNOME supports it, it's the official API
- **Adds latency** - D-Bus round-trip vs. direct uinput
- **Requires user consent** - Portal shows permission dialog
- **Session persistence** - Can be persisted to avoid repeated dialogs

**Recommendation**: Implement RemoteDesktop portal integration. It's the only supported path for GNOME 49+.

---

## Concern 3: "Until It Doesn't Work"

### Known Failure Modes of glupload ! cudaupload

1. **NVIDIA driver version mismatch**
   - Old drivers have broken EGL_EXT_image_dma_buf_import
   - Minimum: 470+ for reasonable support, 525+ recommended

2. **Wayland vs X11**
   - GStreamer's GL context creation differs
   - EGL on Wayland, GLX on X11
   - cudaupload expects EGL context

3. **Modifier negotiation**
   - PipeWire may offer modifiers NVIDIA can't import
   - Need to constrain accepted modifiers

4. **Buffer starvation**
   - PipeWire has limited buffer pool
   - Holding buffers too long causes capture stutter

### Mitigations

```c
// In GStreamer pipeline setup
GstCaps *caps = gst_caps_from_string(
    "video/x-raw(memory:DMABuf),"
    "format={NV12,BGRA},"          // Limit to known-working formats
    "drm-format={NV12,AR24},"
    "width=[1,8192],"
    "height=[1,8192]"
);

// Force LINEAR modifier if needed
gst_caps_set_simple(caps,
    "drm-modifier", G_TYPE_UINT64, DRM_FORMAT_MOD_LINEAR,
    NULL);
```

### Fallback Chain

```
Try: pipewiresrc ! glupload ! cudaupload ! nvh264enc
     │
     ├─ On EGL import failure →
     │   pipewiresrc ! gldownload ! videoconvert ! cudaupload ! nvh264enc
     │
     └─ On total GL failure →
         pipewiresrc ! videoconvert ! x264enc (CPU fallback)
```

---

## Architecture Decision

### Option A: Custom GStreamer Element (gst-pipewire-zerocopy)

**Pros:**
- Maximum control over buffer handling
- Can optimize for Wolf's specific needs
- Reuse some gst-wayland-display code

**Cons:**
- Significant development effort
- Must handle all edge cases ourselves
- Duplicates work GStreamer already does
- Will fail on hybrid graphics without extensive fallback code

**Verdict: NOT RECOMMENDED**

### Option B: Use Standard GStreamer Elements + Smart Pipeline Selection

**Pros:**
- Uses battle-tested GStreamer elements
- Maintained by GStreamer community
- Handles edge cases we haven't thought of
- Less code to maintain in Wolf

**Cons:**
- Less control over exact behavior
- May have overhead from element negotiation

**Verdict: RECOMMENDED**

### Option C: Maintain wl-roots Path for Non-GNOME + PipeWire for GNOME

**Pros:**
- Keep what works for Sway, wl-roots compositors
- Add PipeWire only for GNOME 49+
- Best of both worlds

**Cons:**
- Two code paths to maintain
- Must detect compositor type

**Verdict: RECOMMENDED (in combination with B)**

---

## Proposed Implementation

### Phase 1: PipeWire ScreenCast + RemoteDesktop Integration

```rust
// New module in Wolf: src/pipewire/mod.rs
pub struct PipeWireSession {
    screencast: ScreenCastSession,
    remote_desktop: RemoteDesktopSession,
    pw_stream: PipeWireStream,
}

impl PipeWireSession {
    pub async fn new() -> Result<Self> {
        // Create linked ScreenCast + RemoteDesktop session
        // Return PipeWire stream for video + input injection handles
    }

    pub fn inject_input(&self, event: InputEvent) -> Result<()> {
        // Convert Moonlight input → portal calls
    }
}
```

### Phase 2: GStreamer Pipeline with GPU Detection

```rust
fn create_pipeline(gpu_info: &GpuInfo) -> gst::Pipeline {
    match gpu_info.primary_gpu {
        Gpu::Nvidia { device_id } if gpu_info.screencast_same_device => {
            // Fast path: same GPU
            parse_launch("pipewiresrc ! glupload ! cudaupload ! nvh264enc")
        }
        Gpu::Nvidia { .. } => {
            // Slow path: different GPU, need copy
            parse_launch("pipewiresrc ! gldownload ! cudaupload ! nvh264enc")
        }
        Gpu::Amd | Gpu::Intel => {
            // VAAPI path
            parse_launch("pipewiresrc ! vaapih264enc")
        }
    }
}
```

### Phase 3: Compositor Detection

```rust
fn select_video_source() -> VideoSourceConfig {
    if is_gnome_49_or_later() {
        VideoSourceConfig::PipeWireScreenCast
    } else if is_wlroots_compositor() {
        VideoSourceConfig::WaylandDisplay  // Existing waylanddisplaysrc
    } else {
        VideoSourceConfig::PipeWireScreenCast  // Default for unknown
    }
}
```

---

## Code Reuse Summary

| gst-wayland-display Component | Reuse in PipeWire Path? |
|-------------------------------|-------------------------|
| `wayland-display-core/utils/allocator/cuda/` | Yes - CUDA FFI, but not EGL import |
| `wayland-display-core/utils/device.rs` | Yes - GPU detection |
| `gst-plugin-wayland-display/waylandsrc/` | No - wl-roots specific |
| Build system (cargo-c, Dockerfile) | Yes - pattern reuse |

---

## Open Questions for Wolf Maintainer

1. **Latency tolerance**: Is D-Bus latency for input injection acceptable for gaming? (Typical: 1-5ms added)

2. **Compositor detection**: How should Wolf detect GNOME 49 vs wl-roots? (Wayland protocol checks? Environment variables?)

3. **Fallback behavior**: If PipeWire pipeline fails, should Wolf:
   - Retry with CPU fallback?
   - Show error to user?
   - Attempt wl-roots path anyway?

4. **Session persistence**: Should Wolf persist RemoteDesktop portal sessions to avoid repeated permission dialogs?

---

## Conclusion

The path forward for GNOME 49+ is:

1. **Accept that `pipewiresrc` outputs DMA-BUF** - build robust fallback chains
2. **Use RemoteDesktop portal for input** - it's the only supported API
3. **Keep waylanddisplaysrc for wl-roots** - don't break what works
4. **Reuse gst-wayland-display selectively** - CUDA FFI and device detection, not the EGL import path

The custom zero-copy element (gst-pipewire-zerocopy) is **not recommended** because:
- It would fail on hybrid graphics without extensive fallback code
- Standard GStreamer elements already handle the edge cases
- Development time is better spent on portal integration

**Next Steps:**
1. Review this design with Wolf maintainer
2. Prototype PipeWire + RemoteDesktop portal integration
3. Test `glupload ! cudaupload` pipeline on various hardware
4. Implement compositor detection and pipeline selection
