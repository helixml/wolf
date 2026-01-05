//! PipeWire stream handling - outputs smithay Dmabuf directly

use parking_lot::Mutex;
use pipewire::{context::Context, main_loop::MainLoop, properties::properties, stream::{Stream, StreamFlags}, spa};
use smithay::backend::allocator::{Fourcc, Modifier};
use smithay::backend::allocator::dmabuf::{Dmabuf, DmabufFlags};
use std::os::fd::BorrowedFd;
use std::sync::{atomic::{AtomicBool, Ordering}, mpsc, Arc};
use std::thread::{self, JoinHandle};
use std::time::Duration;

/// Frame received from PipeWire
pub enum FrameData {
    /// DMA-BUF frame (zero-copy) - directly usable with waylanddisplaycore
    DmaBuf(Dmabuf),
    /// SHM fallback
    Shm { data: Vec<u8>, width: u32, height: u32, stride: u32, format: u32 },
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
        let (frame_tx, frame_rx) = mpsc::sync_channel(2);
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

    pub fn recv_frame(&self) -> Result<FrameData, String> {
        if let Some(err) = self.error.lock().take() {
            return Err(err);
        }
        // 30s timeout: GNOME ScreenCast only sends frames when there's damage (screen changes).
        // A static desktop can have long gaps between frames, so we need a generous timeout.
        // See: design/2026-01-05-screenshot-video-pipeline-interference.md
        self.frame_rx.recv_timeout(Duration::from_secs(30))
            .map_err(|e| format!("Failed to receive frame: {}", e))
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

    let props = properties! {
        *pipewire::keys::MEDIA_TYPE => "Video",
        *pipewire::keys::MEDIA_CATEGORY => "Capture",
        *pipewire::keys::MEDIA_ROLE => "Screen",
    };

    let stream = Stream::new(&core, "helix-screencast", props)
        .map_err(|e| format!("Stream: {}", e))?;

    let frame_tx = Arc::new(Mutex::new(frame_tx));
    let video_info_param = video_info.clone();
    let frame_tx_process = frame_tx.clone();

    let _listener = stream
        .add_local_listener_with_user_data(spa::param::video::VideoInfoRaw::default())
        .state_changed(|_, _, old, new| {
            tracing::warn!("[PIPEWIRE_DEBUG] PipeWire state: {:?} -> {:?}", old, new);
        })
        .param_changed(move |_, user_data, id, pod| {
            tracing::warn!("[PIPEWIRE_DEBUG] param_changed called: id={}", id);
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
        })
        .process(move |stream, _| {
            if let Some(mut buffer) = stream.dequeue_buffer() {
                let datas = buffer.datas_mut();
                if datas.is_empty() { return; }

                let params = video_info.lock().clone();
                if let Some(frame) = extract_frame(datas, &params) {
                    let _ = frame_tx_process.lock().try_send(frame);
                }
            }
        })
        .register()
        .map_err(|e| format!("Listener: {}", e))?;

    // Create empty params slice
    let params: &mut [&spa::pod::Pod] = &mut [];

    stream.connect(
        pipewire::spa::utils::Direction::Input,
        Some(node_id),
        StreamFlags::AUTOCONNECT | StreamFlags::MAP_BUFFERS,
        params,
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
