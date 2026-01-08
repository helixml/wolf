//! PipeWire stream handling - outputs smithay Dmabuf directly

use pipewire::{context::Context, main_loop::MainLoop, properties::properties, stream::{Stream, StreamFlags}, spa};
use pipewire::spa::pod::{Object, Property, PropertyFlags, Value, ChoiceValue};
use pipewire::spa::utils::{Choice, ChoiceFlags, ChoiceEnum, Fraction, Id, SpaTypes};
use pipewire::spa::param::ParamType;
use pipewire::spa::param::format::{FormatProperties, MediaType, MediaSubtype};
use pipewire::spa::pod::serialize::PodSerializer;
use pipewire::spa::pod::Pod;
use parking_lot::Mutex;
use smithay::backend::allocator::{Fourcc, Modifier};
use smithay::backend::allocator::dmabuf::{Dmabuf, DmabufFlags};
use std::io::Cursor;
use std::os::fd::BorrowedFd;
use std::sync::{atomic::{AtomicBool, Ordering}, mpsc};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::Duration;

/// Frame received from PipeWire
pub enum FrameData {
    /// DMA-BUF frame (zero-copy) - directly usable with waylanddisplaycore
    DmaBuf(Dmabuf),
    /// SHM fallback
    Shm { data: Vec<u8>, width: u32, height: u32, stride: u32, format: u32 },
}

/// Error type for frame receive operations
#[derive(Debug, Clone)]
pub enum RecvError {
    /// Timeout waiting for frame (normal for damage-based GNOME ScreenCast)
    Timeout,
    /// Channel disconnected
    Disconnected,
    /// Other error with message
    Error(String),
}

impl std::fmt::Display for RecvError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            RecvError::Timeout => write!(f, "timeout waiting for frame"),
            RecvError::Disconnected => write!(f, "channel disconnected"),
            RecvError::Error(msg) => write!(f, "{}", msg),
        }
    }
}

/// Video parameters from PipeWire format negotiation
#[derive(Debug, Clone, Default)]
pub struct VideoParams {
    pub width: u32,
    pub height: u32,
    pub format: u32,
    pub modifier: u64,
}

/// PipeWire stream wrapper
pub struct PipeWireStream {
    thread: Option<JoinHandle<()>>,
    frame_rx: mpsc::Receiver<FrameData>,
    shutdown: Arc<AtomicBool>,
    video_info: Arc<Mutex<VideoParams>>,
    error: Arc<Mutex<Option<String>>>,
}

impl PipeWireStream {
    pub fn connect(node_id: u32) -> Result<Self, String> {
        // Use larger buffer to reduce backpressure from GStreamer consumer
        // This helps prevent frame drops when GNOME delivers frames faster than we consume them
        let (frame_tx, frame_rx) = mpsc::sync_channel(8);
        let shutdown = Arc::new(AtomicBool::new(false));
        let shutdown_clone = shutdown.clone();
        let video_info = Arc::new(Mutex::new(VideoParams::default()));
        let video_info_clone = video_info.clone();
        let error = Arc::new(Mutex::new(None));
        let error_clone = error.clone();

        let thread = thread::Builder::new()
            .name("pipewire-stream".to_string())
            .spawn(move || {
                if let Err(e) = run_pipewire_loop(node_id, frame_tx, shutdown_clone, video_info_clone) {
                    tracing::error!("PipeWire loop error: {}", e);
                    *error_clone.lock() = Some(e);
                }
            })
            .map_err(|e| format!("Failed to spawn PipeWire thread: {}", e))?;

        thread::sleep(Duration::from_millis(100));

        if let Some(err) = error.lock().take() {
            shutdown.store(true, Ordering::SeqCst);
            return Err(err);
        }

        Ok(PipeWireStream { thread: Some(thread), frame_rx, shutdown, video_info, error })
    }

    /// Receive a frame with the default timeout (30 seconds).
    /// For GNOME ScreenCast damage-based delivery, use `recv_frame_timeout` with keepalive.
    pub fn recv_frame(&self) -> Result<FrameData, RecvError> {
        self.recv_frame_timeout(Duration::from_secs(30))
    }

    /// Receive a frame with a configurable timeout.
    ///
    /// Returns `RecvError::Timeout` if no frame arrives within the timeout.
    /// This is normal for GNOME 49+ damage-based ScreenCast - static desktops
    /// produce no frames. Callers should implement keepalive by resending the
    /// last buffer when timeout occurs.
    ///
    /// See: design/2026-01-06-pipewire-keepalive-mechanism.md
    pub fn recv_frame_timeout(&self, timeout: Duration) -> Result<FrameData, RecvError> {
        if let Some(err) = self.error.lock().take() {
            return Err(RecvError::Error(err));
        }
        self.frame_rx.recv_timeout(timeout)
            .map_err(|e| match e {
                mpsc::RecvTimeoutError::Timeout => RecvError::Timeout,
                mpsc::RecvTimeoutError::Disconnected => RecvError::Disconnected,
            })
    }

    #[allow(dead_code)]
    pub fn video_params(&self) -> VideoParams {
        self.video_info.lock().clone()
    }
}

impl Drop for PipeWireStream {
    fn drop(&mut self) {
        self.shutdown.store(true, Ordering::SeqCst);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

/// Convert SPA VideoFormat to DRM fourcc code
/// SPA format enum values: https://docs.pipewire.org/spa_2param_2video_2format_8h.html
fn spa_video_format_to_drm_fourcc(format: spa::param::video::VideoFormat) -> u32 {
    // DRM fourcc codes (little-endian):
    // AR24 = 0x34325241 = ARGB8888
    // AB24 = 0x34324241 = ABGR8888
    // XR24 = 0x34325258 = XRGB8888
    // XB24 = 0x34324258 = XBGR8888
    // RA24 = 0x34324152 = RGBA8888
    // BA24 = 0x34324142 = BGRA8888
    // RX24 = 0x34325852 = RGBX8888
    // BX24 = 0x34325842 = BGRX8888
    // NV12 = 0x3231564e = NV12
    match format {
        spa::param::video::VideoFormat::BGRA => 0x34324142, // BA24 = BGRA8888
        spa::param::video::VideoFormat::RGBA => 0x34324152, // RA24 = RGBA8888
        // BGRx/RGBx: Use ARGB/ABGR for CUDA compatibility
        // CUDA rejects both XRGB8888 and BGRX8888 with NVIDIA tiled modifiers.
        // Use formats with alpha channels (ARGB/ABGR) which CUDA accepts.
        // BGRx bytes: B,G,R,x -> ARGB8888 bytes: B,G,R,A (same layout, alpha treated as opaque)
        // RGBx bytes: R,G,B,x -> ABGR8888 bytes: R,G,B,A (same layout, alpha treated as opaque)
        spa::param::video::VideoFormat::BGRx => 0x34325241, // AR24 = ARGB8888 (CUDA accepts with tiled modifiers)
        spa::param::video::VideoFormat::RGBx => 0x34324241, // AB24 = ABGR8888 (CUDA accepts with tiled modifiers)
        spa::param::video::VideoFormat::ARGB => 0x34325241, // AR24 = ARGB8888
        spa::param::video::VideoFormat::ABGR => 0x34324241, // AB24 = ABGR8888
        spa::param::video::VideoFormat::xRGB => 0x34325258, // XR24 = XRGB8888
        spa::param::video::VideoFormat::xBGR => 0x34324258, // XB24 = XBGR8888
        spa::param::video::VideoFormat::NV12 => 0x3231564e, // NV12
        spa::param::video::VideoFormat::I420 => 0x32315549, // I420
        _ => {
            tracing::warn!("Unknown SPA video format {:?}, defaulting to ARGB8888", format);
            0x34325241 // AR24 = ARGB8888
        }
    }
}

fn run_pipewire_loop(
    node_id: u32,
    frame_tx: mpsc::SyncSender<FrameData>,
    shutdown: Arc<AtomicBool>,
    video_info: Arc<Mutex<VideoParams>>,
) -> Result<(), String> {
    pipewire::init();

    let mainloop = MainLoop::new(None).map_err(|e| format!("MainLoop: {}", e))?;
    let context = Context::new(&mainloop).map_err(|e| format!("Context: {}", e))?;
    let core = context.connect(None).map_err(|e| format!("Connect: {}", e))?;

    // Request high framerate in stream properties
    // VIDEO_RATE hints to PipeWire what framerate we want
    let props = properties! {
        *pipewire::keys::MEDIA_TYPE => "Video",
        *pipewire::keys::MEDIA_CATEGORY => "Capture",
        *pipewire::keys::MEDIA_ROLE => "Screen",
        *pipewire::keys::VIDEO_RATE => "60/1",
    };

    let stream = Stream::new(&core, "helix-screencast", props)
        .map_err(|e| format!("Stream: {}", e))?;

    // Note: SyncSender is thread-safe, no Mutex needed
    let video_info_param = video_info.clone();
    let frame_tx_process = frame_tx.clone();

    let _listener = stream
        .add_local_listener_with_user_data(spa::param::video::VideoInfoRaw::default())
        .state_changed(|_, _, old, new| {
            eprintln!("[PIPEWIRE_DEBUG] PipeWire state: {:?} -> {:?}", old, new);
        })
        .param_changed(move |stream, user_data, id, pod| {
            eprintln!("[PIPEWIRE_DEBUG] param_changed called: id={}", id);
            if id != spa::param::ParamType::Format.as_raw() {
                return;
            }
            let Some(param) = pod else {
                tracing::warn!("[PIPEWIRE_DEBUG] param_changed: pod is None");
                return;
            };

            // Parse media type and subtype
            let (media_type, media_subtype) = match spa::param::format_utils::parse_format(param) {
                Ok(v) => v,
                Err(e) => {
                    tracing::warn!("[PIPEWIRE_DEBUG] Failed to parse format: {:?}", e);
                    return;
                }
            };

            tracing::warn!("[PIPEWIRE_DEBUG] media_type={:?} media_subtype={:?}", media_type, media_subtype);

            // We only handle video/raw
            if media_type != spa::param::format::MediaType::Video
                || media_subtype != spa::param::format::MediaSubtype::Raw
            {
                tracing::warn!("[PIPEWIRE_DEBUG] Ignoring non-raw video format");
                return;
            }

            // Parse the VideoInfoRaw from the pod
            if let Err(e) = user_data.parse(param) {
                tracing::warn!("[PIPEWIRE_DEBUG] Failed to parse VideoInfoRaw: {:?}", e);
                return;
            }

            let width = user_data.size().width;
            let height = user_data.size().height;
            let format_raw = user_data.format().as_raw();

            tracing::warn!(
                "[PIPEWIRE_DEBUG] PipeWire video format: {}x{} format={} ({:?}) framerate={}/{}",
                width,
                height,
                format_raw,
                user_data.format(),
                user_data.framerate().num,
                user_data.framerate().denom
            );

            // Update VideoParams - convert SPA video format to DRM fourcc
            // SPA formats: BGRA=2, RGBA=4, BGRx=5, RGBx=6, ARGB=7, ABGR=8, xRGB=9, xBGR=10
            // See: https://pipewire.pages.freedesktop.org/pipewire/group__spa__param.html
            let drm_fourcc = spa_video_format_to_drm_fourcc(user_data.format());
            let modifier = user_data.modifier();
            tracing::warn!(
                "[PIPEWIRE_DEBUG] Converted to DRM fourcc: 0x{:x}, modifier: 0x{:x}",
                drm_fourcc, modifier
            );
            let mut params = video_info_param.lock();
            params.width = width;
            params.height = height;
            params.format = drm_fourcc;
            params.modifier = modifier;
            drop(params); // Release lock before update_params

            // Tell PipeWire we accept DMA-BUF buffers (like OBS does in on_param_changed_cb)
            // Without this, PipeWire defaults to SHM (MemPtr) frames which break CUDA path
            let buffer_params = build_buffer_params();
            if !buffer_params.is_empty() {
                if let Some(pod) = Pod::from_bytes(&buffer_params) {
                    eprintln!("[PIPEWIRE_DEBUG] Calling update_params to request DMA-BUF buffers");
                    if let Err(e) = stream.update_params(&mut [pod]) {
                        eprintln!("[PIPEWIRE_DEBUG] update_params failed: {:?}", e);
                    } else {
                        eprintln!("[PIPEWIRE_DEBUG] update_params succeeded - DMA-BUF buffers requested");
                    }
                }
            }
        })
        .process(move |stream, _| {
            // Use a static counter for frame stats logging
            use std::sync::atomic::{AtomicU64, AtomicBool, Ordering};
            static FRAME_COUNT: AtomicU64 = AtomicU64::new(0);
            static LAST_LOG: AtomicU64 = AtomicU64::new(0);
            static LOGGED_START: AtomicBool = AtomicBool::new(false);

            if let Some(mut buffer) = stream.dequeue_buffer() {
                let datas = buffer.datas_mut();
                if datas.is_empty() { return; }

                let params = video_info.lock().clone();
                if let Some(frame) = extract_frame(datas, &params) {
                    let count = FRAME_COUNT.fetch_add(1, Ordering::Relaxed) + 1;

                    // Log first frame
                    if !LOGGED_START.swap(true, Ordering::Relaxed) {
                        tracing::warn!("[PIPEWIRE_FRAME] First frame received from PipeWire ({}x{})",
                            params.width, params.height);
                    }

                    // Log every 100th frame or every 5 seconds
                    let now = std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .map(|d| d.as_secs())
                        .unwrap_or(0);
                    let last = LAST_LOG.load(Ordering::Relaxed);
                    if count % 100 == 0 || (now > last + 5) {
                        LAST_LOG.store(now, Ordering::Relaxed);
                        tracing::warn!("[PIPEWIRE_FRAME] Frame #{} received from PipeWire", count);
                    }

                    let _ = frame_tx_process.try_send(frame);
                }
            }
        })
        .register()
        .map_err(|e| format!("Listener: {}", e))?;

    // Build format params with framerate range (like OBS does)
    // This tells PipeWire we accept video/raw with framerate 0-360 fps
    // Without this, PipeWire may throttle to a lower framerate
    let format_params = build_video_format_params();
    let params_pod = Pod::from_bytes(&format_params)
        .ok_or_else(|| "Failed to create Pod from format params".to_string())?;
    let mut params = [params_pod];

    tracing::info!("Connecting to PipeWire node {} with framerate range 0-360fps", node_id);

    stream.connect(
        pipewire::spa::utils::Direction::Input,
        Some(node_id),
        StreamFlags::AUTOCONNECT | StreamFlags::MAP_BUFFERS,
        &mut params,
    ).map_err(|e| format!("Connect to node {}: {}", node_id, e))?;

    tracing::info!("Connected to PipeWire node {}", node_id);

    while !shutdown.load(Ordering::SeqCst) {
        mainloop.loop_().iterate(Duration::from_millis(50));
    }

    Ok(())
}

/// Extract DRM fourcc code from SPA video format - exposed for testing
pub fn spa_format_to_drm_fourcc(format: spa::param::video::VideoFormat) -> u32 {
    spa_video_format_to_drm_fourcc(format)
}

/// Build buffer params to request DMA-BUF buffer support.
/// Without this, PipeWire sends SHM (MemPtr) frames which break CUDA zero-copy path.
/// OBS does the same in on_param_changed_cb - it tells PipeWire to send DMA-BUF buffers.
/// See: https://github.com/obsproject/obs-studio/blob/master/plugins/linux-pipewire/pipewire.c
fn build_buffer_params() -> Vec<u8> {
    // SPA_PARAM_BUFFERS_dataType property key (from spa/param/buffers.h)
    // enum spa_param_buffers: START=0, buffers=1, blocks=2, size=3, stride=4, align=5, dataType=6
    const SPA_PARAM_BUFFERS_DATATYPE: u32 = 6;

    // Buffer type bitmask (from spa/buffer/buffer.h):
    // SPA_DATA_MemPtr = 1 (pointer to memory)
    // SPA_DATA_DmaBuf = 3 (DMA-BUF fd)
    // Bitmask: (1 << 1) | (1 << 3) = 2 | 8 = 10
    let buffer_types: i32 = (1 << 1) | (1 << 3);

    // Create property for dataType
    let mut properties = Vec::new();
    properties.push(Property {
        key: SPA_PARAM_BUFFERS_DATATYPE,
        flags: PropertyFlags::empty(),
        value: Value::Int(buffer_types),
    });

    // Create the buffers param object
    let obj = Object {
        type_: SpaTypes::ObjectParamBuffers.as_raw(),
        id: ParamType::Buffers.as_raw(),
        properties,
    };

    // Serialize to bytes
    let result = PodSerializer::serialize(
        Cursor::new(Vec::new()),
        &Value::Object(obj),
    );

    match result {
        Ok((cursor, _len)) => cursor.into_inner(),
        Err(e) => {
            eprintln!("[PIPEWIRE_DEBUG] Failed to serialize buffer params: {:?}", e);
            Vec::new()
        }
    }
}

/// Build a single format pod (like OBS's build_format function).
/// Creates one EnumFormat pod for a specific video format with optional modifier.
fn build_single_format_pod(format: u32, with_modifier: bool) -> Vec<u8> {
    let mut properties = Vec::new();

    // Media type: Video
    properties.push(Property {
        key: FormatProperties::MediaType.as_raw(),
        flags: PropertyFlags::empty(),
        value: Value::Id(Id(MediaType::Video.as_raw())),
    });

    // Media subtype: Raw
    properties.push(Property {
        key: FormatProperties::MediaSubtype.as_raw(),
        flags: PropertyFlags::empty(),
        value: Value::Id(Id(MediaSubtype::Raw.as_raw())),
    });

    // Video format (single specific format, not an enum)
    properties.push(Property {
        key: FormatProperties::VideoFormat.as_raw(),
        flags: PropertyFlags::empty(),
        value: Value::Id(Id(format)),
    });

    // Modifier for DMA-BUF (like OBS does)
    // DRM_FORMAT_MOD_INVALID = ((1ULL << 56) - 1) = 0x00ffffffffffffff
    // This is the "implicit modifier" that tells the driver to choose
    if with_modifier {
        const DRM_FORMAT_MOD_INVALID: i64 = ((1i64 << 56) - 1);

        // OBS uses MANDATORY | DONT_FIXATE flags
        // MANDATORY (0x08): This property must be present in negotiated format
        // DONT_FIXATE (0x10): Don't lock to a single value during negotiation
        let modifier_flags = PropertyFlags::from_bits_retain(0x18);

        // Build an enum choice with the modifier
        // For enum, first value is preferred/default, rest are alternatives
        let modifier_choice = Choice(
            ChoiceFlags::empty(),
            ChoiceEnum::Enum {
                default: DRM_FORMAT_MOD_INVALID,
                alternatives: vec![DRM_FORMAT_MOD_INVALID],
            },
        );
        properties.push(Property {
            key: FormatProperties::VideoModifier.as_raw(),
            flags: modifier_flags,
            value: Value::Choice(ChoiceValue::Long(modifier_choice)),
        });
    }

    // Framerate: Range from 0/1 to 360/1, default 60/1
    let framerate_choice = Choice(
        ChoiceFlags::empty(),
        ChoiceEnum::Range {
            default: Fraction { num: 60, denom: 1 },
            min: Fraction { num: 0, denom: 1 },
            max: Fraction { num: 360, denom: 1 },
        },
    );
    properties.push(Property {
        key: FormatProperties::VideoFramerate.as_raw(),
        flags: PropertyFlags::empty(),
        value: Value::Choice(ChoiceValue::Fraction(framerate_choice)),
    });

    // Create the format object
    let obj = Object {
        type_: SpaTypes::ObjectParamFormat.as_raw(),
        id: ParamType::EnumFormat.as_raw(),
        properties,
    };

    // Serialize to bytes
    match PodSerializer::serialize(Cursor::new(Vec::new()), &Value::Object(obj)) {
        Ok((cursor, _len)) => cursor.into_inner(),
        Err(e) => {
            tracing::error!("Failed to serialize format pod: {:?}", e);
            Vec::new()
        }
    }
}

/// Build video format params following OBS's pattern.
/// Creates multiple format pods: first with modifiers (DMA-BUF), then without (SHM fallback).
/// Returns a list of serialized pods that can be concatenated and passed to stream.connect().
fn build_video_format_params() -> Vec<u8> {
    // Supported formats (same as OBS's supported_formats_sync)
    // SPA_VIDEO_FORMAT values from spa/param/video/format.h:
    // BGRA=2, RGBA=4, BGRx=5, RGBx=6
    const SPA_VIDEO_FORMAT_BGRA: u32 = 2;
    const SPA_VIDEO_FORMAT_RGBA: u32 = 4;
    const SPA_VIDEO_FORMAT_BGRX: u32 = 5;
    const SPA_VIDEO_FORMAT_RGBX: u32 = 6;

    let formats = [
        SPA_VIDEO_FORMAT_BGRA,
        SPA_VIDEO_FORMAT_RGBA,
        SPA_VIDEO_FORMAT_BGRX,
        SPA_VIDEO_FORMAT_RGBX,
    ];

    // First, build pods WITH modifiers (DMA-BUF capable)
    // Then, build pods WITHOUT modifiers (SHM fallback)
    // PipeWire will try them in order and use the first one that works
    //
    // NOTE: For now, we just build one simple format pod without modifiers
    // to ensure basic negotiation works. We'll add modifier support after
    // confirming the pipeline works.

    // Build the first format with modifier (DMA-BUF)
    let pod = build_single_format_pod(SPA_VIDEO_FORMAT_BGRX, true);
    if !pod.is_empty() {
        eprintln!("[PIPEWIRE_DEBUG] Built format pod for BGRx with modifier ({} bytes)", pod.len());
        return pod;
    }

    // Fallback: build without modifier (SHM)
    eprintln!("[PIPEWIRE_DEBUG] Falling back to format pod without modifier");
    build_single_format_pod(SPA_VIDEO_FORMAT_BGRX, false)
}

fn extract_frame(datas: &mut [pipewire::spa::buffer::Data], params: &VideoParams) -> Option<FrameData> {
    // Get chunk info from first element before we need mutable access
    let (size, stride, data_type, fd, offset) = {
        let first = datas.first()?;
        let chunk = first.chunk();
        (
            chunk.size() as usize,
            chunk.stride(),
            first.type_(),
            first.as_raw().fd as i32,
            chunk.offset(),
        )
    };
    if size == 0 { return None; }

    // DMA-BUF path - build smithay Dmabuf directly
    if data_type == pipewire::spa::buffer::DataType::DmaBuf {
        if fd < 0 { return None; }

        // Use params from format negotiation, fallback to calculated values only if not set
        let width = if params.width > 0 { params.width } else { (stride / 4) as u32 };
        let height = if params.height > 0 { params.height } else if stride > 0 { (size as u32) / (stride as u32) } else { 0 };

        // Use WARN level to ensure visibility with RUST_LOG=WARN
        tracing::warn!(
            "[PIPEWIRE_DEBUG] extract_frame: params={}x{} format=0x{:x} modifier=0x{:x}, chunk: size={} stride={} offset={}, fd={}",
            params.width, params.height, params.format, params.modifier, size, stride, offset, fd
        );

        if width == 0 || height == 0 { return None; }

        // Build smithay Dmabuf from PipeWire buffer info
        let fourcc = Fourcc::try_from(params.format).unwrap_or(Fourcc::Argb8888);
        // Use the modifier from PipeWire format negotiation:
        // - 0x0 (Linear) = explicit linear layout, most common
        // - 0xffffffffffffff (Invalid) = implicit modifier, let driver decide
        // - Other values = explicit tiled/compressed formats (GPU-specific)
        // We pass through whatever PipeWire negotiated; EGL/CUDA will reject incompatible formats
        let modifier = Modifier::from(params.modifier);
        tracing::warn!("[PIPEWIRE_DEBUG] Using fourcc={:?} modifier={:?} (raw: 0x{:x})", fourcc, modifier, params.modifier);

        // Clone the fd to create OwnedFd
        let owned_fd = unsafe {
            BorrowedFd::borrow_raw(fd).try_clone_to_owned().ok()?
        };

        let mut builder = Dmabuf::builder((width as i32, height as i32), fourcc, modifier, DmabufFlags::empty());
        builder.add_plane(owned_fd, 0, offset, stride as u32);

        // Add additional planes if present
        for (idx, data) in datas.iter().enumerate().skip(1) {
            if data.type_() == pipewire::spa::buffer::DataType::DmaBuf {
                let raw = data.as_raw();
                let plane_fd_raw = raw.fd as i32;
                if plane_fd_raw >= 0 {
                    let plane_fd = unsafe {
                        BorrowedFd::borrow_raw(plane_fd_raw).try_clone_to_owned().ok()?
                    };
                    builder.add_plane(plane_fd, idx as u32, data.chunk().offset(), data.chunk().stride() as u32);
                }
            }
        }

        if let Some(dmabuf) = builder.build() {
            tracing::debug!("DMA-BUF frame: {}x{}", width, height);
            return Some(FrameData::DmaBuf(dmabuf));
        }
    }

    // SHM fallback - need mutable access for data()
    if let Some(first_mut) = datas.first_mut() {
        if let Some(data_ptr) = first_mut.data() {
            let width = if params.width > 0 { params.width } else if stride > 0 { (stride / 4) as u32 } else { 0 };
            let height = if params.height > 0 { params.height } else if stride > 0 { (size / stride as usize) as u32 } else { 0 };
            if width == 0 || height == 0 { return None; }

            let data = unsafe { std::slice::from_raw_parts(data_ptr.as_ptr(), size) }.to_vec();
            return Some(FrameData::Shm { data, width, height, stride: stride as u32, format: params.format });
        }
    }

    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use pipewire::spa;

    /// Test SPA video format to DRM fourcc conversion for common formats
    #[test]
    fn test_spa_to_drm_fourcc_bgra() {
        let fourcc = spa_video_format_to_drm_fourcc(spa::param::video::VideoFormat::BGRA);
        // BA24 = BGRA8888 = 0x34324142
        assert_eq!(fourcc, 0x34324142, "BGRA should map to BA24");
    }

    #[test]
    fn test_spa_to_drm_fourcc_rgba() {
        let fourcc = spa_video_format_to_drm_fourcc(spa::param::video::VideoFormat::RGBA);
        // RA24 = RGBA8888 = 0x34324152
        assert_eq!(fourcc, 0x34324152, "RGBA should map to RA24");
    }

    #[test]
    fn test_spa_to_drm_fourcc_bgrx() {
        // BGRx maps to ARGB8888 for CUDA compatibility
        // CUDA rejects XRGB8888 and BGRX8888 with tiled modifiers, but accepts ARGB8888
        let fourcc = spa_video_format_to_drm_fourcc(spa::param::video::VideoFormat::BGRx);
        assert_eq!(fourcc, 0x34325241, "BGRx should map to ARGB8888 (AR24) for CUDA compatibility");
    }

    #[test]
    fn test_spa_to_drm_fourcc_rgbx() {
        // RGBx maps to ABGR8888 for CUDA compatibility
        // CUDA rejects XBGR8888 and RGBX8888 with tiled modifiers, but accepts ABGR8888
        let fourcc = spa_video_format_to_drm_fourcc(spa::param::video::VideoFormat::RGBx);
        assert_eq!(fourcc, 0x34324241, "RGBx should map to ABGR8888 (AB24) for CUDA compatibility");
    }

    #[test]
    fn test_spa_to_drm_fourcc_argb() {
        let fourcc = spa_video_format_to_drm_fourcc(spa::param::video::VideoFormat::ARGB);
        // AR24 = ARGB8888 = 0x34325241
        assert_eq!(fourcc, 0x34325241, "ARGB should map to AR24");
    }

    #[test]
    fn test_spa_to_drm_fourcc_abgr() {
        let fourcc = spa_video_format_to_drm_fourcc(spa::param::video::VideoFormat::ABGR);
        // AB24 = ABGR8888 = 0x34324241
        assert_eq!(fourcc, 0x34324241, "ABGR should map to AB24");
    }

    #[test]
    fn test_spa_to_drm_fourcc_xrgb() {
        // xRGB should map to XRGB8888 (XR24)
        let fourcc = spa_video_format_to_drm_fourcc(spa::param::video::VideoFormat::xRGB);
        assert_eq!(fourcc, 0x34325258, "xRGB should map to XRGB8888 (XR24)");
    }

    #[test]
    fn test_spa_to_drm_fourcc_xbgr() {
        // xBGR should map to XBGR8888 (XB24)
        let fourcc = spa_video_format_to_drm_fourcc(spa::param::video::VideoFormat::xBGR);
        assert_eq!(fourcc, 0x34324258, "xBGR should map to XBGR8888 (XB24)");
    }

    #[test]
    fn test_spa_to_drm_fourcc_nv12() {
        let fourcc = spa_video_format_to_drm_fourcc(spa::param::video::VideoFormat::NV12);
        // NV12 = 0x3231564e
        assert_eq!(fourcc, 0x3231564e, "NV12 should map correctly");
    }

    #[test]
    fn test_spa_to_drm_fourcc_i420() {
        let fourcc = spa_video_format_to_drm_fourcc(spa::param::video::VideoFormat::I420);
        // I420 = 0x32315549
        assert_eq!(fourcc, 0x32315549, "I420 should map correctly");
    }

    /// Test VideoParams default values
    #[test]
    fn test_video_params_default() {
        let params = VideoParams::default();
        assert_eq!(params.width, 0);
        assert_eq!(params.height, 0);
        assert_eq!(params.format, 0);
        assert_eq!(params.modifier, 0);
    }
}
