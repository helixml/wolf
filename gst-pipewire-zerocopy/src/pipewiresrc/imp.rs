//! PipeWire ScreenCast source - reuses waylanddisplaycore's CUDA conversion
//!
//! This element follows Wolf/gst-wayland-display's context sharing pattern:
//! - Wolf creates the CUDA context and pushes it via set_context()
//! - We receive it in set_context() and store it
//! - We respond to context queries from downstream elements
//! - Fallback: if no context pushed, acquire via new_from_gstreamer()

use crate::pipewire_stream::{FrameData, PipeWireStream};
use gst::glib;
use gst::prelude::*;
use gst::subclass::prelude::*;
use gst_base::prelude::BaseSrcExt;
use gst_base::subclass::base_src::CreateSuccess;
use gst_base::subclass::prelude::*;
use gst_video::{VideoCapsBuilder, VideoFormat, VideoInfo, VideoInfoDmaDrm};
use once_cell::sync::Lazy;
use parking_lot::Mutex;
use smithay::backend::allocator::Buffer;
use smithay::backend::drm::{DrmNode, NodeType};
use smithay::backend::egl::{EGLDevice, EGLDisplay};
use smithay::backend::egl::ffi::egl::types::EGLDisplay as RawEGLDisplay;
use std::sync::atomic::AtomicPtr;
use std::sync::Arc;

// Reuse battle-tested CUDA code from waylanddisplaycore
use waylanddisplaycore::utils::allocator::cuda::{
    init_cuda, CUDAContext, CUDAImage, EGLImage, CUDABufferPool, GstCudaContext,
    CAPS_FEATURE_MEMORY_CUDA_MEMORY, gst_cuda_handle_context_query_wrapped,
};

static CAT: Lazy<gst::DebugCategory> = Lazy::new(|| {
    gst::DebugCategory::new("pipewirezerocopysrc", gst::DebugColorFlags::empty(), Some("PipeWire zero-copy source"))
});

/// Create EGL display from render node path (reuses waylanddisplaycore's pattern)
fn create_egl_display(node_path: &str) -> Result<EGLDisplay, String> {
    let drm_node = DrmNode::from_path(node_path).map_err(|e| format!("DrmNode: {:?}", e))?;
    let drm_render = drm_node.node_with_type(NodeType::Render).and_then(Result::ok).unwrap_or(drm_node);
    let device = EGLDevice::enumerate().map_err(|e| format!("enumerate: {:?}", e))?
        .find(|d| d.try_get_render_node().unwrap_or_default() == Some(drm_render.clone()))
        .ok_or_else(|| "No EGLDevice for node".to_string())?;
    unsafe { EGLDisplay::new(device).map_err(|e| format!("EGLDisplay: {:?}", e)) }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum OutputMode { #[default] Auto, Cuda, DmaBuf, System }

impl OutputMode {
    fn from_str(s: &str) -> Self {
        match s.to_lowercase().as_str() {
            "cuda" => Self::Cuda,
            "dmabuf" | "dma-buf" => Self::DmaBuf,
            "system" | "memory" | "shm" => Self::System,
            _ => Self::Auto,
        }
    }
}

#[derive(Debug)]
pub struct Settings {
    pipewire_node_id: Option<u32>,
    render_node: Option<String>,
    output_mode: OutputMode,
    cuda_device_id: i32,
    /// CUDA context received from Wolf via set_context() or acquired via GStreamer
    cuda_context: Option<Arc<std::sync::Mutex<CUDAContext>>>,
    /// Raw pointer for GStreamer CUDA context interop - used by Wolf's context sharing
    cuda_raw_ptr: AtomicPtr<GstCudaContext>,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            pipewire_node_id: None,
            render_node: Some("/dev/dri/renderD128".into()),
            output_mode: OutputMode::Auto,
            cuda_device_id: -1,
            cuda_context: None,
            cuda_raw_ptr: AtomicPtr::new(std::ptr::null_mut()),
        }
    }
}

pub struct State {
    stream: Option<PipeWireStream>,
    video_info: Option<VideoInfo>,
    egl_display: Option<Arc<EGLDisplay>>,
    buffer_pool: Option<CUDABufferPool>,
    actual_output_mode: OutputMode,
    frame_count: u64,
}

impl Default for State {
    fn default() -> Self {
        Self { stream: None, video_info: None, egl_display: None, buffer_pool: None, actual_output_mode: OutputMode::System, frame_count: 0 }
    }
}

pub struct PipeWireZeroCopySrc {
    settings: Mutex<Settings>,
    state: Mutex<Option<State>>,
}

impl Default for PipeWireZeroCopySrc {
    fn default() -> Self {
        Self { settings: Mutex::new(Settings::default()), state: Mutex::new(None) }
    }
}

#[glib::object_subclass]
impl ObjectSubclass for PipeWireZeroCopySrc {
    const NAME: &'static str = "GstPipeWireZeroCopySrc";
    type Type = super::PipeWireZeroCopySrc;
    type ParentType = gst_base::PushSrc;
}

impl ObjectImpl for PipeWireZeroCopySrc {
    fn properties() -> &'static [glib::ParamSpec] {
        static PROPERTIES: Lazy<Vec<glib::ParamSpec>> = Lazy::new(|| vec![
            glib::ParamSpecUInt::builder("pipewire-node-id").nick("PipeWire Node ID").blurb("PipeWire node ID from ScreenCast portal").construct().build(),
            glib::ParamSpecString::builder("render-node").nick("DRM Render Node").blurb("DRM render node").default_value(Some("/dev/dri/renderD128")).construct().build(),
            glib::ParamSpecString::builder("output-mode").nick("Output Mode").blurb("auto, cuda, dmabuf, or system").default_value(Some("auto")).construct().build(),
            glib::ParamSpecInt::builder("cuda-device-id").nick("CUDA Device ID").blurb("CUDA device ID (-1 for auto)").minimum(-1).maximum(16).default_value(-1).construct().build(),
        ]);
        PROPERTIES.as_ref()
    }

    fn set_property(&self, _id: usize, value: &glib::Value, pspec: &glib::ParamSpec) {
        let mut s = self.settings.lock();
        match pspec.name() {
            "pipewire-node-id" => s.pipewire_node_id = Some(value.get().unwrap()),
            "render-node" => s.render_node = value.get().unwrap(),
            "output-mode" => s.output_mode = value.get::<Option<String>>().unwrap().as_deref().map(OutputMode::from_str).unwrap_or_default(),
            "cuda-device-id" => s.cuda_device_id = value.get().unwrap(),
            _ => {}
        }
    }

    fn property(&self, _id: usize, pspec: &glib::ParamSpec) -> glib::Value {
        let s = self.settings.lock();
        match pspec.name() {
            "pipewire-node-id" => s.pipewire_node_id.unwrap_or(0).to_value(),
            "render-node" => s.render_node.clone().unwrap_or_else(|| "/dev/dri/renderD128".into()).to_value(),
            "output-mode" => match s.output_mode { OutputMode::Auto => "auto", OutputMode::Cuda => "cuda", OutputMode::DmaBuf => "dmabuf", OutputMode::System => "system" }.to_value(),
            "cuda-device-id" => s.cuda_device_id.to_value(),
            _ => unreachable!(),
        }
    }

    fn constructed(&self) {
        self.parent_constructed();
        let obj = self.obj();
        obj.set_element_flags(gst::ElementFlags::SOURCE);
        obj.set_live(true);
        obj.set_format(gst::Format::Time);
        obj.set_automatic_eos(false);
        obj.set_do_timestamp(true);
    }
}

impl GstObjectImpl for PipeWireZeroCopySrc {}

impl ElementImpl for PipeWireZeroCopySrc {
    fn metadata() -> Option<&'static gst::subclass::ElementMetadata> {
        static META: Lazy<gst::subclass::ElementMetadata> = Lazy::new(|| {
            gst::subclass::ElementMetadata::new("PipeWire Zero-Copy Source", "Source/Video", "Captures PipeWire ScreenCast with zero-copy GPU output", "Wolf Project")
        });
        Some(&*META)
    }

    fn pad_templates() -> &'static [gst::PadTemplate] {
        static TEMPLATES: Lazy<Vec<gst::PadTemplate>> = Lazy::new(|| {
            let mut caps = gst::Caps::new_empty();
            caps.merge(VideoCapsBuilder::new().features([CAPS_FEATURE_MEMORY_CUDA_MEMORY]).format_list([VideoFormat::Bgra, VideoFormat::Rgba, VideoFormat::Nv12]).build());
            caps.merge(VideoCapsBuilder::new().features([gstreamer_allocators::CAPS_FEATURE_MEMORY_DMABUF]).format(VideoFormat::DmaDrm).build());
            caps.merge(VideoCapsBuilder::new().format_list([VideoFormat::Bgra, VideoFormat::Rgba]).build());
            vec![gst::PadTemplate::new("src", gst::PadDirection::Src, gst::PadPresence::Always, &caps).unwrap()]
        });
        TEMPLATES.as_ref()
    }

    fn change_state(&self, transition: gst::StateChange) -> Result<gst::StateChangeSuccess, gst::StateChangeError> {
        match self.parent_change_state(transition) {
            Ok(gst::StateChangeSuccess::Success) if transition.next() == gst::State::Paused => Ok(gst::StateChangeSuccess::NoPreroll),
            x => x,
        }
    }

    /// Receive CUDA context from Wolf via GStreamer's context mechanism.
    /// Wolf creates the context and pushes it to elements via gst_element_set_context().
    fn set_context(&self, context: &gst::Context) {
        let elem = self.obj().upcast_ref::<gst::Element>().to_owned();

        // Get raw pointer for GStreamer CUDA interop
        let cuda_raw_ptr = {
            let settings = self.settings.lock();
            settings.cuda_raw_ptr.as_ptr()
        };

        // Try to create CUDA context from the pushed context
        // This matches Wolf's pattern: it creates context and pushes it to elements
        match CUDAContext::new_from_set_context(&elem, context, -1, cuda_raw_ptr) {
            Ok(ctx) => {
                let mut settings = self.settings.lock();
                if settings.cuda_context.is_none() {
                    gst::info!(CAT, imp = self, "Received CUDA context from pipeline (via set_context)");
                    settings.cuda_context = Some(Arc::new(std::sync::Mutex::new(ctx)));
                }
            }
            Err(e) => {
                gst::debug!(CAT, imp = self, "set_context: not a CUDA context or failed: {}", e);
            }
        }

        self.parent_set_context(context)
    }
}

impl BaseSrcImpl for PipeWireZeroCopySrc {
    fn start(&self) -> Result<(), gst::ErrorMessage> {
        let mut state_guard = self.state.lock();
        if state_guard.is_some() { return Ok(()); }

        // Extract settings - but don't hold the lock while doing CUDA setup
        let (node_id, render_node, output_mode, device_id) = {
            let settings = self.settings.lock();
            let node_id = settings.pipewire_node_id.ok_or_else(|| gst::error_msg!(gst::LibraryError::Settings, ("pipewire-node-id must be set")))?;
            (node_id, settings.render_node.clone(), settings.output_mode, settings.cuda_device_id)
        };

        gst::info!(CAT, imp = self, "Starting for node {}", node_id);

        let mut state = State::default();
        let elem = self.obj().upcast_ref::<gst::Element>().to_owned();

        // Try CUDA mode using Wolf's context sharing pattern
        if output_mode == OutputMode::Auto || output_mode == OutputMode::Cuda {
            if let Ok(()) = init_cuda() {
                // Check if we already received a CUDA context via set_context()
                let have_cuda_context = self.settings.lock().cuda_context.is_some();

                if !have_cuda_context {
                    // No context pushed by Wolf - try to acquire one from the pipeline
                    // This matches waylandsrc's fallback behavior
                    gst::info!(CAT, imp = self, "No CUDA context from set_context, acquiring from pipeline");
                    let cuda_raw_ptr = self.settings.lock().cuda_raw_ptr.as_ptr();
                    match CUDAContext::new_from_gstreamer(&elem, device_id, cuda_raw_ptr) {
                        Ok(ctx) => {
                            let mut settings = self.settings.lock();
                            if settings.cuda_context.is_none() {
                                gst::info!(CAT, imp = self, "Acquired CUDA context via new_from_gstreamer");
                                settings.cuda_context = Some(Arc::new(std::sync::Mutex::new(ctx)));
                            }
                        }
                        Err(e) => {
                            gst::warning!(CAT, imp = self, "Failed to acquire CUDA context: {}", e);
                        }
                    }
                }

                // Now check if we have a context and set up EGL/buffer pool
                let settings = self.settings.lock();
                if let Some(ref cuda_context) = settings.cuda_context {
                    if let Some(ref node_path) = render_node {
                        match create_egl_display(node_path) {
                            Ok(display) => {
                                let cuda_ctx = cuda_context.lock().unwrap();
                                if let Ok(pool) = CUDABufferPool::new(&cuda_ctx) {
                                    gst::info!(CAT, imp = self, "Using CUDA mode with shared context");
                                    state.egl_display = Some(Arc::new(display));
                                    state.buffer_pool = Some(pool);
                                    state.actual_output_mode = OutputMode::Cuda;
                                }
                            }
                            Err(e) => gst::warning!(CAT, imp = self, "EGL display failed: {}", e),
                        }
                    }
                }
            }
        }

        if state.actual_output_mode == OutputMode::System {
            if output_mode == OutputMode::DmaBuf {
                state.actual_output_mode = OutputMode::DmaBuf;
                gst::info!(CAT, imp = self, "Using DMA-BUF mode");
            } else {
                gst::info!(CAT, imp = self, "Using system memory mode");
            }
        }

        let stream = PipeWireStream::connect(node_id).map_err(|e| gst::error_msg!(gst::LibraryError::Init, ("PipeWire: {}", e)))?;
        state.stream = Some(stream);
        *state_guard = Some(state);
        Ok(())
    }

    fn stop(&self) -> Result<(), gst::ErrorMessage> {
        let mut g = self.state.lock();
        if let Some(s) = g.take() {
            gst::info!(CAT, imp = self, "Stopping");
            drop(s);
        }
        Ok(())
    }

    fn is_seekable(&self) -> bool { false }

    /// Handle context queries from downstream elements.
    /// When downstream elements need a CUDA context, we provide ours.
    fn query(&self, query: &mut gst::QueryRef) -> bool {
        if query.type_() == gst::QueryType::Context {
            let settings = self.settings.lock();
            if let Some(ref cuda_context) = settings.cuda_context {
                gst::debug!(CAT, imp = self, "Handling CUDA context query from downstream");
                let cuda_ctx = cuda_context.lock().unwrap();
                return gst_cuda_handle_context_query_wrapped(
                    self.obj().as_ref().as_ref(),
                    query,
                    &cuda_ctx,
                );
            }
        }
        BaseSrcImplExt::parent_query(self, query)
    }

    fn caps(&self, filter: Option<&gst::Caps>) -> Option<gst::Caps> {
        let g = self.state.lock();
        let mut caps = match g.as_ref().map(|s| s.actual_output_mode) {
            Some(OutputMode::Cuda) => VideoCapsBuilder::new().features([CAPS_FEATURE_MEMORY_CUDA_MEMORY]).format_list([VideoFormat::Bgra, VideoFormat::Rgba, VideoFormat::Nv12]).build(),
            Some(OutputMode::DmaBuf) => VideoCapsBuilder::new().features([gstreamer_allocators::CAPS_FEATURE_MEMORY_DMABUF]).format(VideoFormat::DmaDrm).build(),
            _ => VideoCapsBuilder::new().format_list([VideoFormat::Bgra, VideoFormat::Rgba]).build(),
        };
        if let Some(f) = filter { caps = caps.intersect(f); }
        Some(caps)
    }

    fn set_caps(&self, caps: &gst::Caps) -> Result<(), gst::LoggableError> {
        gst::info!(CAT, imp = self, "Caps: {:?}", caps);
        if let Ok(info) = VideoInfo::from_caps(caps) {
            if let Some(s) = self.state.lock().as_mut() { s.video_info = Some(info); }
        }
        self.parent_set_caps(caps)
    }
}

impl PushSrcImpl for PipeWireZeroCopySrc {
    fn create(&self, _buffer: Option<&mut gst::BufferRef>) -> Result<CreateSuccess, gst::FlowError> {
        // Get shared CUDA context from settings (cloning the Arc, not the context)
        let cuda_context = self.settings.lock().cuda_context.clone();

        let mut g = self.state.lock();
        let state = g.as_mut().ok_or(gst::FlowError::Eos)?;
        let stream = state.stream.as_ref().ok_or(gst::FlowError::Error)?;

        let frame = stream.recv_frame().map_err(|e| { gst::error!(CAT, imp = self, "Frame: {}", e); gst::FlowError::Error })?;

        let buffer = match frame {
            FrameData::DmaBuf(dmabuf) if state.actual_output_mode == OutputMode::Cuda => {
                // Use waylanddisplaycore's battle-tested CUDA conversion with shared context
                let cuda_context_arc = cuda_context.as_ref().ok_or(gst::FlowError::Error)?;
                let cuda_ctx = cuda_context_arc.lock().unwrap();
                let egl_display = state.egl_display.as_ref().ok_or(gst::FlowError::Error)?;

                // Get raw EGLDisplay handle for waylanddisplaycore's EGLImage::from()
                let raw_display: RawEGLDisplay = egl_display.get_display_handle().handle;

                let egl_image = EGLImage::from(&dmabuf, &raw_display)
                    .map_err(|e| { gst::error!(CAT, imp = self, "EGLImage: {}", e); gst::FlowError::Error })?;

                let cuda_image = CUDAImage::from(egl_image, &cuda_ctx)
                    .map_err(|e| { gst::error!(CAT, imp = self, "CUDAImage: {}", e); gst::FlowError::Error })?;

                // Create VideoInfoDmaDrm using the new() constructor
                let base_info = VideoInfo::builder(VideoFormat::Bgra, dmabuf.width() as u32, dmabuf.height() as u32)
                    .build()
                    .map_err(|_| gst::FlowError::Error)?;
                let fourcc: u32 = dmabuf.format().code as u32;
                let modifier: u64 = dmabuf.format().modifier.into();
                let video_info = VideoInfoDmaDrm::new(base_info, fourcc, modifier);

                cuda_image.to_gst_buffer(video_info, &cuda_ctx, state.buffer_pool.as_ref())
                    .map_err(|e| { gst::error!(CAT, imp = self, "CUDA buffer: {}", e); gst::FlowError::Error })?
            }
            FrameData::DmaBuf(dmabuf) => {
                // SHM fallback - mmap and copy
                self.dmabuf_to_system(&dmabuf)?
            }
            FrameData::Shm { data, width, height, stride, .. } => {
                self.create_system_buffer(&data, width, height, stride)?
            }
        };

        state.frame_count += 1;
        Ok(CreateSuccess::NewBuffer(buffer))
    }
}

impl PipeWireZeroCopySrc {
    fn dmabuf_to_system(&self, dmabuf: &smithay::backend::allocator::dmabuf::Dmabuf) -> Result<gst::Buffer, gst::FlowError> {
        use std::os::fd::AsRawFd;
        let size = (dmabuf.height() as usize) * (dmabuf.strides().next().unwrap_or(0) as usize);
        let mut data = vec![0u8; size];

        unsafe {
            let fd = dmabuf.handles().next().ok_or(gst::FlowError::Error)?.as_raw_fd();
            let ptr = libc::mmap(std::ptr::null_mut(), size, libc::PROT_READ, libc::MAP_SHARED, fd, 0);
            if ptr == libc::MAP_FAILED { return Err(gst::FlowError::Error); }
            std::ptr::copy_nonoverlapping(ptr as *const u8, data.as_mut_ptr(), size);
            libc::munmap(ptr, size);
        }

        self.create_system_buffer(&data, dmabuf.width() as u32, dmabuf.height() as u32, dmabuf.strides().next().unwrap_or(0))
    }

    fn create_system_buffer(&self, data: &[u8], width: u32, height: u32, stride: u32) -> Result<gst::Buffer, gst::FlowError> {
        let mut buffer = gst::Buffer::with_size(data.len()).map_err(|_| gst::FlowError::Error)?;
        {
            let buf = buffer.get_mut().unwrap();
            buf.map_writable().map_err(|_| gst::FlowError::Error)?.copy_from_slice(data);
        }
        {
            let buf = buffer.get_mut().unwrap();
            gst_video::VideoMeta::add_full(buf, gst_video::VideoFrameFlags::empty(), VideoFormat::Bgra, width, height, &[0], &[stride as i32]).map_err(|_| gst::FlowError::Error)?;
        }
        Ok(buffer)
    }
}
