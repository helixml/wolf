//! PipeWire stream handling for ScreenCast capture
//!
//! This module manages the PipeWire connection and frame extraction from
//! ScreenCast streams.

use crate::dmabuf::DmaBuf;
use libspa::pod::Pod;
use parking_lot::Mutex;
use pipewire::{
    context::Context,
    main_loop::MainLoop,
    spa::{
        self,
        param::video::{VideoFormat, VideoInfoRaw},
        pod::Object,
        utils::Direction,
    },
    stream::{Stream, StreamFlags, StreamState},
};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    mpsc::{self, Receiver, Sender, TryRecvError},
    Arc,
};
use std::thread::{self, JoinHandle};

/// Frame data received from PipeWire
#[derive(Debug)]
pub enum FrameData {
    /// DMA-BUF frame (zero-copy path)
    DmaBuf(DmaBuf),
    /// Shared memory frame (fallback path)
    Shm {
        data: Vec<u8>,
        width: u32,
        height: u32,
        stride: u32,
        format: u32,
    },
}

/// PipeWire stream state
pub struct PipeWireStream {
    /// Thread running the PipeWire main loop
    thread: Option<JoinHandle<()>>,
    /// Channel to receive frames
    frame_rx: Receiver<FrameData>,
    /// Shutdown signal
    shutdown: Arc<AtomicBool>,
    /// Current video info (updated on param change)
    video_info: Arc<Mutex<Option<VideoParams>>>,
}

/// Video stream parameters
#[derive(Debug, Clone)]
pub struct VideoParams {
    pub width: u32,
    pub height: u32,
    pub format: VideoFormat,
    pub framerate_num: u32,
    pub framerate_denom: u32,
}

impl PipeWireStream {
    /// Connect to a PipeWire ScreenCast node
    pub fn connect(node_id: u32) -> Result<Self, String> {
        let (frame_tx, frame_rx) = mpsc::channel();
        let shutdown = Arc::new(AtomicBool::new(false));
        let shutdown_clone = shutdown.clone();
        let video_info = Arc::new(Mutex::new(None));
        let video_info_clone = video_info.clone();

        let thread = thread::spawn(move || {
            if let Err(e) = run_pipewire_loop(node_id, frame_tx, shutdown_clone, video_info_clone) {
                tracing::error!("PipeWire loop error: {}", e);
            }
        });

        Ok(PipeWireStream {
            thread: Some(thread),
            frame_rx,
            shutdown,
            video_info,
        })
    }

    /// Try to receive the next frame (non-blocking)
    pub fn try_recv_frame(&self) -> Result<Option<FrameData>, String> {
        match self.frame_rx.try_recv() {
            Ok(frame) => Ok(Some(frame)),
            Err(TryRecvError::Empty) => Ok(None),
            Err(TryRecvError::Disconnected) => Err("PipeWire stream disconnected".to_string()),
        }
    }

    /// Receive the next frame (blocking)
    pub fn recv_frame(&self) -> Result<FrameData, String> {
        self.frame_rx
            .recv()
            .map_err(|_| "PipeWire stream disconnected".to_string())
    }

    /// Get current video parameters
    pub fn video_params(&self) -> Option<VideoParams> {
        self.video_info.lock().clone()
    }

    /// Check if stream is still running
    pub fn is_running(&self) -> bool {
        !self.shutdown.load(Ordering::SeqCst)
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

/// Run the PipeWire main loop
fn run_pipewire_loop(
    node_id: u32,
    frame_tx: Sender<FrameData>,
    shutdown: Arc<AtomicBool>,
    video_info: Arc<Mutex<Option<VideoParams>>>,
) -> Result<(), String> {
    // Initialize PipeWire
    pipewire::init();

    let mainloop = MainLoop::new(None).map_err(|e| format!("Failed to create MainLoop: {}", e))?;
    let context = Context::new(&mainloop).map_err(|e| format!("Failed to create Context: {}", e))?;
    let core = context
        .connect(None)
        .map_err(|e| format!("Failed to connect to PipeWire: {}", e))?;

    // Create stream
    let stream = Stream::new(
        &core,
        "helix-screencast",
        pipewire::properties! {
            *pipewire::keys::MEDIA_TYPE => "Video",
            *pipewire::keys::MEDIA_CATEGORY => "Capture",
            *pipewire::keys::MEDIA_ROLE => "Screen",
        },
    )
    .map_err(|e| format!("Failed to create stream: {}", e))?;

    // Build format parameters - request DMA-BUF with SHM fallback
    let params = build_format_params();

    // Create listener data
    let listener_data = Arc::new(Mutex::new(ListenerData {
        frame_tx: frame_tx.clone(),
        video_info: video_info.clone(),
        current_format: None,
    }));

    let listener_data_process = listener_data.clone();
    let listener_data_param = listener_data.clone();

    let _listener = stream
        .add_local_listener_with_user_data(())
        .state_changed(|_, _, old, new| {
            tracing::debug!("Stream state changed: {:?} -> {:?}", old, new);
        })
        .param_changed(move |_, _, id, param| {
            if id == spa::param::ParamType::Format.as_raw() {
                if let Some(param) = param {
                    handle_format_change(&listener_data_param, param);
                }
            }
        })
        .process(move |stream, _| {
            handle_process(&listener_data_process, stream);
        })
        .register()
        .map_err(|e| format!("Failed to register listener: {}", e))?;

    // Connect to the node
    stream
        .connect(
            Direction::Input,
            Some(node_id),
            StreamFlags::AUTOCONNECT | StreamFlags::MAP_BUFFERS,
            &mut params.iter().map(|p| p.as_ref()).collect::<Vec<_>>(),
        )
        .map_err(|e| format!("Failed to connect stream: {}", e))?;

    tracing::info!("Connected to PipeWire node {}", node_id);

    // Run main loop
    while !shutdown.load(Ordering::SeqCst) {
        mainloop.iterate(std::time::Duration::from_millis(100));
    }

    Ok(())
}

struct ListenerData {
    frame_tx: Sender<FrameData>,
    video_info: Arc<Mutex<Option<VideoParams>>>,
    current_format: Option<VideoParams>,
}

fn build_format_params() -> Vec<Vec<u8>> {
    // For now, return empty params to accept any format
    // In a full implementation, we'd build proper SPA pod params
    // requesting DMA-BUF with modifiers
    vec![]
}

fn handle_format_change(data: &Arc<Mutex<ListenerData>>, _param: &Pod) {
    // Parse the format from the pod
    // This would extract width, height, format, framerate from the SPA pod
    // For now, we'll handle this in the process callback by inspecting buffers

    tracing::debug!("Format changed");
}

fn handle_process(data: &Arc<Mutex<ListenerData>>, stream: &Stream) {
    let mut data = data.lock();

    // Dequeue buffer
    let buffer = match stream.dequeue_buffer() {
        Some(b) => b,
        None => return,
    };

    let buf = buffer.buffer();
    let datas = buf.datas();

    if datas.is_empty() {
        tracing::warn!("Empty buffer received");
        return;
    }

    // Check buffer type
    let spa_data = &datas[0];

    // Get data type from the chunk
    let chunk = match spa_data.chunk() {
        Some(c) => c,
        None => {
            tracing::warn!("No chunk in buffer");
            return;
        }
    };

    // For now, handle as SHM (memory-mapped)
    // DMA-BUF handling requires checking spa_data.type_() == SPA_DATA_DmaBuf
    if let Some(data_ptr) = spa_data.data() {
        let size = chunk.size() as usize;
        let stride = chunk.stride() as u32;

        // Copy the data (SHM path)
        let frame_data = unsafe {
            std::slice::from_raw_parts(data_ptr as *const u8, size)
        };

        // Get dimensions from meta or format
        // For now use placeholder values - real implementation parses from format
        let width = stride / 4; // Assuming 4 bytes per pixel
        let height = size as u32 / stride;

        let frame = FrameData::Shm {
            data: frame_data.to_vec(),
            width,
            height,
            stride,
            format: 0, // DRM_FORMAT_XRGB8888
        };

        if data.frame_tx.send(frame).is_err() {
            tracing::warn!("Failed to send frame");
        }
    }

    // Queue buffer back
    // Note: In pipewire-rs 0.8, buffer is automatically queued when dropped
}

/// Extract DMA-BUF from a PipeWire SPA buffer
///
/// # Safety
/// The spa_buffer must be valid and contain DMA-BUF data
pub unsafe fn extract_dmabuf_from_spa_buffer(
    spa_buffer: *const libspa::sys::spa_buffer,
    width: u32,
    height: u32,
    fourcc: u32,
    modifier: u64,
) -> Result<DmaBuf, &'static str> {
    if spa_buffer.is_null() {
        return Err("Null SPA buffer");
    }

    let buffer = &*spa_buffer;
    let n_datas = buffer.n_datas as usize;

    if n_datas == 0 || n_datas > 4 {
        return Err("Invalid plane count");
    }

    let datas = std::slice::from_raw_parts(buffer.datas, n_datas);

    let mut fds = Vec::with_capacity(n_datas);
    let mut offsets = Vec::with_capacity(n_datas);
    let mut strides = Vec::with_capacity(n_datas);

    for data in datas {
        // Check if this is a DMA-BUF
        // SPA_DATA_DmaBuf = 2
        if data.type_ != 2 {
            return Err("Not a DMA-BUF");
        }

        fds.push(data.fd as i32);

        // Get offset and stride from chunk if available
        if !data.chunk.is_null() {
            let chunk = &*data.chunk;
            offsets.push(chunk.offset);
            strides.push(chunk.stride as u32);
        } else {
            offsets.push(0);
            strides.push(0);
        }
    }

    DmaBuf::from_raw(width, height, fourcc, modifier, &fds, &offsets, &strides)
}
