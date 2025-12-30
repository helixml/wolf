//! PipeWire stream handling - outputs smithay Dmabuf directly

use parking_lot::Mutex;
use pipewire::{context::Context, main_loop::MainLoop, stream::{Stream, StreamFlags}};
use smithay::backend::allocator::{Fourcc, Modifier};
use smithay::backend::allocator::dmabuf::{Dmabuf, DmabufFlags};
use std::os::fd::{BorrowedFd, OwnedFd};
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
        self.frame_rx.recv_timeout(Duration::from_secs(5))
            .map_err(|e| format!("Failed to receive frame: {}", e))
    }

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

    let stream = Stream::new(
        &core, "helix-screencast",
        pipewire::properties! {
            *pipewire::keys::MEDIA_TYPE => "Video",
            *pipewire::keys::MEDIA_CATEGORY => "Capture",
            *pipewire::keys::MEDIA_ROLE => "Screen",
        },
    ).map_err(|e| format!("Stream: {}", e))?;

    let frame_tx = Arc::new(Mutex::new(frame_tx));
    let video_info_cb = video_info.clone();
    let frame_tx_process = frame_tx.clone();

    let _listener = stream
        .add_local_listener_with_user_data(())
        .state_changed(|_, _, old, new| {
            tracing::info!("PipeWire state: {:?} -> {:?}", old, new);
        })
        .param_changed(move |_, _, id, pod| {
            if id == libspa::param::ParamType::Format.as_raw() {
                if let Some(_pod) = pod {
                    // TODO: Parse format from pod - for now rely on buffer metadata
                }
            }
        })
        .process(move |stream, _| {
            if let Some(mut buffer) = stream.dequeue_buffer() {
                let datas = buffer.buffer().datas();
                if datas.is_empty() { return; }

                let params = video_info.lock().clone();
                if let Some(frame) = extract_frame(&datas, &params) {
                    let _ = frame_tx_process.lock().try_send(frame);
                }
            }
        })
        .register()
        .map_err(|e| format!("Listener: {}", e))?;

    stream.connect(
        pipewire::spa::utils::Direction::Input,
        Some(node_id),
        StreamFlags::AUTOCONNECT | StreamFlags::MAP_BUFFERS,
        &mut [],
    ).map_err(|e| format!("Connect to node {}: {}", node_id, e))?;

    tracing::info!("Connected to PipeWire node {}", node_id);

    while !shutdown.load(Ordering::SeqCst) {
        mainloop.iterate(Duration::from_millis(50));
    }

    Ok(())
}

fn extract_frame(datas: &[pipewire::spa::buffer::Data], params: &VideoParams) -> Option<FrameData> {
    let first = datas.first()?;
    let chunk = first.chunk();
    let size = chunk.size() as usize;
    let stride = chunk.stride();
    if size == 0 { return None; }

    let data_type = first.type_();

    // DMA-BUF path - build smithay Dmabuf directly
    if data_type == pipewire::spa::buffer::DataType::DmaBuf {
        let fd = first.as_raw().fd as i32;
        if fd < 0 { return None; }

        let width = if params.width > 0 { params.width } else { (stride / 4) as u32 };
        let height = if params.height > 0 { params.height } else if stride > 0 { (size as u32) / (stride as u32) } else { 0 };
        if width == 0 || height == 0 { return None; }

        // Build smithay Dmabuf from PipeWire buffer info
        let fourcc = Fourcc::try_from(params.format).unwrap_or(Fourcc::Argb8888);
        let modifier = Modifier::from(params.modifier);

        // Clone the fd to create OwnedFd
        let owned_fd = unsafe {
            OwnedFd::from(BorrowedFd::borrow_raw(fd).try_clone_to_owned().ok()?)
        };

        let mut builder = Dmabuf::builder((width as i32, height as i32), fourcc, modifier, DmabufFlags::empty());
        builder.add_plane(owned_fd, 0, chunk.offset(), stride as u32);

        // Add additional planes if present
        for (idx, data) in datas.iter().enumerate().skip(1) {
            if data.type_() == pipewire::spa::buffer::DataType::DmaBuf {
                let raw = data.as_raw();
                let plane_fd = unsafe {
                    BorrowedFd::borrow_raw(raw.fd as i32).try_clone_to_owned().ok()?
                };
                builder.add_plane(plane_fd, idx as u32, data.chunk().offset(), data.chunk().stride() as u32);
            }
        }

        if let Some(dmabuf) = builder.build() {
            tracing::debug!("DMA-BUF frame: {}x{}", width, height);
            return Some(FrameData::DmaBuf(dmabuf));
        }
    }

    // SHM fallback
    if let Some(data_ptr) = first.data() {
        let width = if params.width > 0 { params.width } else if stride > 0 { (stride / 4) as u32 } else { 0 };
        let height = if params.height > 0 { params.height } else if stride > 0 { (size / stride as usize) as u32 } else { 0 };
        if width == 0 || height == 0 { return None; }

        let data = unsafe { std::slice::from_raw_parts(data_ptr.as_ptr(), size) }.to_vec();
        return Some(FrameData::Shm { data, width, height, stride: stride as u32, format: params.format });
    }

    None
}
