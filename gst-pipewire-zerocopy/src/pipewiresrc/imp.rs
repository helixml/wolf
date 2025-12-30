//! PipeWire ScreenCast source element implementation
//!
//! This element captures video from PipeWire ScreenCast and outputs buffers in the
//! most efficient format available:
//! - CUDA memory for NVIDIA GPUs
//! - DMA-BUF for AMD/Intel GPUs
//! - System memory as fallback

use crate::cuda;
use crate::pipewire_stream::{FrameData, PipeWireStream};
use gst::glib;
use gst::prelude::*;
use gst::subclass::prelude::*;
use gst_base::prelude::BaseSrcExt;
use gst_base::subclass::base_src::CreateSuccess;
use gst_base::subclass::prelude::*;
use gst_video::{VideoCapsBuilder, VideoFormat, VideoInfo};
use once_cell::sync::Lazy;
use parking_lot::Mutex;
use std::sync::Arc;

#[cfg(feature = "cuda")]
use waylanddisplaycore::utils::allocator::cuda::CAPS_FEATURE_MEMORY_CUDA_MEMORY;

/// Logging category for this element
static CAT: Lazy<gst::DebugCategory> = Lazy::new(|| {
    gst::DebugCategory::new(
        "pipewirezerocopysrc",
        gst::DebugColorFlags::empty(),
        Some("PipeWire zero-copy source"),
    )
});

/// Output mode for the element
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum OutputMode {
    /// Auto-detect best output mode
    #[default]
    Auto,
    /// Force CUDA output (NVIDIA)
    Cuda,
    /// Force DMA-BUF output (AMD/Intel)
    DmaBuf,
    /// Force system memory output
    System,
}

impl OutputMode {
    fn from_str(s: &str) -> Self {
        match s.to_lowercase().as_str() {
            "cuda" => OutputMode::Cuda,
            "dmabuf" | "dma-buf" => OutputMode::DmaBuf,
            "system" | "memory" | "shm" => OutputMode::System,
            _ => OutputMode::Auto,
        }
    }
}

/// Element settings (configured via properties)
#[derive(Debug)]
pub struct Settings {
    /// PipeWire node ID to connect to (from ScreenCast portal)
    pipewire_node_id: Option<u32>,
    /// DRM render node for GPU operations
    render_node: Option<String>,
    /// Output mode preference
    output_mode: OutputMode,
    /// CUDA device ID (-1 for auto)
    cuda_device_id: i32,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            pipewire_node_id: None,
            render_node: Some("/dev/dri/renderD128".to_string()),
            output_mode: OutputMode::Auto,
            cuda_device_id: -1,
        }
    }
}

/// Element runtime state
pub struct State {
    /// PipeWire stream
    stream: Option<PipeWireStream>,
    /// Current video info
    video_info: Option<VideoInfo>,
    /// CUDA context (if using CUDA output)
    #[cfg(feature = "cuda")]
    cuda_context: Option<cuda::CudaContext>,
    /// Detected output mode
    actual_output_mode: OutputMode,
    /// Frame counter for timestamps
    frame_count: u64,
}

impl Default for State {
    fn default() -> Self {
        Self {
            stream: None,
            video_info: None,
            #[cfg(feature = "cuda")]
            cuda_context: None,
            actual_output_mode: OutputMode::System,
            frame_count: 0,
        }
    }
}

/// The PipeWire zero-copy source element
pub struct PipeWireZeroCopySrc {
    settings: Mutex<Settings>,
    state: Mutex<Option<State>>,
}

impl Default for PipeWireZeroCopySrc {
    fn default() -> Self {
        Self {
            settings: Mutex::new(Settings::default()),
            state: Mutex::new(None),
        }
    }
}

#[glib::object_subclass]
impl ObjectSubclass for PipeWireZeroCopySrc {
    const NAME: &'static str = "GstPipeWireZeroCopySrc";
    type Type = super::PipeWireZeroCopySrc;
    type ParentType = gst_base::PushSrc;
    type Interfaces = ();
}

impl ObjectImpl for PipeWireZeroCopySrc {
    fn properties() -> &'static [glib::ParamSpec] {
        static PROPERTIES: Lazy<Vec<glib::ParamSpec>> = Lazy::new(|| {
            vec![
                glib::ParamSpecUInt::builder("pipewire-node-id")
                    .nick("PipeWire Node ID")
                    .blurb("PipeWire node ID from ScreenCast portal")
                    .construct()
                    .build(),
                glib::ParamSpecString::builder("render-node")
                    .nick("DRM Render Node")
                    .blurb("DRM render node for GPU operations (e.g. /dev/dri/renderD128)")
                    .default_value(Some("/dev/dri/renderD128"))
                    .construct()
                    .build(),
                glib::ParamSpecString::builder("output-mode")
                    .nick("Output Mode")
                    .blurb("Output buffer mode: auto, cuda, dmabuf, or system")
                    .default_value(Some("auto"))
                    .construct()
                    .build(),
                glib::ParamSpecInt::builder("cuda-device-id")
                    .nick("CUDA Device ID")
                    .blurb("CUDA device ID to use (-1 for auto)")
                    .minimum(-1)
                    .maximum(16)
                    .default_value(-1)
                    .construct()
                    .build(),
            ]
        });

        PROPERTIES.as_ref()
    }

    fn set_property(&self, _id: usize, value: &glib::Value, pspec: &glib::ParamSpec) {
        let mut settings = self.settings.lock();
        match pspec.name() {
            "pipewire-node-id" => {
                settings.pipewire_node_id = Some(value.get().expect("Type checked upstream"));
            }
            "render-node" => {
                settings.render_node = value.get().expect("Type checked upstream");
            }
            "output-mode" => {
                let mode_str: Option<String> = value.get().expect("Type checked upstream");
                settings.output_mode = mode_str
                    .as_deref()
                    .map(OutputMode::from_str)
                    .unwrap_or_default();
            }
            "cuda-device-id" => {
                settings.cuda_device_id = value.get().expect("Type checked upstream");
            }
            _ => unreachable!(),
        }
    }

    fn property(&self, _id: usize, pspec: &glib::ParamSpec) -> glib::Value {
        let settings = self.settings.lock();
        match pspec.name() {
            "pipewire-node-id" => settings.pipewire_node_id.unwrap_or(0).to_value(),
            "render-node" => settings
                .render_node
                .clone()
                .unwrap_or_else(|| "/dev/dri/renderD128".to_string())
                .to_value(),
            "output-mode" => match settings.output_mode {
                OutputMode::Auto => "auto",
                OutputMode::Cuda => "cuda",
                OutputMode::DmaBuf => "dmabuf",
                OutputMode::System => "system",
            }
            .to_value(),
            "cuda-device-id" => settings.cuda_device_id.to_value(),
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
        static ELEMENT_METADATA: Lazy<gst::subclass::ElementMetadata> = Lazy::new(|| {
            gst::subclass::ElementMetadata::new(
                "PipeWire Zero-Copy Source",
                "Source/Video",
                "Captures PipeWire ScreenCast with zero-copy GPU output",
                "Wolf Project <https://github.com/games-on-whales/wolf>",
            )
        });

        Some(&*ELEMENT_METADATA)
    }

    fn pad_templates() -> &'static [gst::PadTemplate] {
        static PAD_TEMPLATES: Lazy<Vec<gst::PadTemplate>> = Lazy::new(|| {
            // Build caps supporting all output modes
            let mut caps = gst::Caps::new_empty();

            // CUDA output (NVIDIA) - highest priority
            #[cfg(feature = "cuda")]
            {
                let cuda_caps = VideoCapsBuilder::new()
                    .features([CAPS_FEATURE_MEMORY_CUDA_MEMORY])
                    .format_list([VideoFormat::Bgra, VideoFormat::Rgba, VideoFormat::Nv12])
                    .build();
                caps.merge(cuda_caps);
            }

            // DMA-BUF output (AMD/Intel)
            let dmabuf_caps = VideoCapsBuilder::new()
                .features([gstreamer_allocators::CAPS_FEATURE_MEMORY_DMABUF])
                .format(VideoFormat::DmaDrm)
                .build();
            caps.merge(dmabuf_caps);

            // System memory output (fallback)
            let sys_caps = VideoCapsBuilder::new()
                .format_list([VideoFormat::Bgra, VideoFormat::Rgba, VideoFormat::Bgrx])
                .build();
            caps.merge(sys_caps);

            let src_pad_template = gst::PadTemplate::new(
                "src",
                gst::PadDirection::Src,
                gst::PadPresence::Always,
                &caps,
            )
            .unwrap();

            vec![src_pad_template]
        });

        PAD_TEMPLATES.as_ref()
    }

    fn change_state(
        &self,
        transition: gst::StateChange,
    ) -> Result<gst::StateChangeSuccess, gst::StateChangeError> {
        let res = self.parent_change_state(transition);
        match res {
            Ok(gst::StateChangeSuccess::Success) => {
                if transition.next() == gst::State::Paused {
                    // Live source: no preroll
                    Ok(gst::StateChangeSuccess::NoPreroll)
                } else {
                    Ok(gst::StateChangeSuccess::Success)
                }
            }
            x => x,
        }
    }
}

impl BaseSrcImpl for PipeWireZeroCopySrc {
    fn start(&self) -> Result<(), gst::ErrorMessage> {
        let mut state_guard = self.state.lock();
        if state_guard.is_some() {
            return Ok(());
        }

        let settings = self.settings.lock();
        let node_id = settings.pipewire_node_id.ok_or_else(|| {
            gst::error_msg!(
                gst::LibraryError::Settings,
                ("pipewire-node-id property must be set")
            )
        })?;

        gst::info!(CAT, imp = self, "Starting PipeWire source for node {}", node_id);

        // Determine output mode
        let mut state = State::default();

        // Try CUDA if requested or auto
        #[cfg(feature = "cuda")]
        if settings.output_mode == OutputMode::Auto || settings.output_mode == OutputMode::Cuda {
            if cuda::is_cuda_available() {
                let device_id = if settings.cuda_device_id >= 0 {
                    settings.cuda_device_id
                } else {
                    0
                };

                match cuda::CudaContext::new(device_id) {
                    Ok(ctx) => {
                        gst::info!(CAT, imp = self, "Using CUDA output mode (device {})", device_id);
                        state.cuda_context = Some(ctx);
                        state.actual_output_mode = OutputMode::Cuda;
                    }
                    Err(e) => {
                        gst::warning!(CAT, imp = self, "Failed to create CUDA context: {}", e);
                        if settings.output_mode == OutputMode::Cuda {
                            return Err(gst::error_msg!(
                                gst::LibraryError::Init,
                                ("Failed to create CUDA context: {}", e)
                            ));
                        }
                    }
                }
            }
        }

        // Fall back to DMA-BUF or system memory
        if state.actual_output_mode == OutputMode::System {
            if settings.output_mode == OutputMode::DmaBuf {
                state.actual_output_mode = OutputMode::DmaBuf;
                gst::info!(CAT, imp = self, "Using DMA-BUF output mode");
            } else {
                gst::info!(CAT, imp = self, "Using system memory output mode");
            }
        }

        drop(settings);

        // Connect to PipeWire
        let stream = PipeWireStream::connect(node_id).map_err(|e| {
            gst::error_msg!(
                gst::LibraryError::Init,
                ("Failed to connect to PipeWire: {}", e)
            )
        })?;

        state.stream = Some(stream);
        *state_guard = Some(state);

        Ok(())
    }

    fn stop(&self) -> Result<(), gst::ErrorMessage> {
        let mut state_guard = self.state.lock();
        if let Some(state) = state_guard.take() {
            gst::info!(CAT, imp = self, "Stopping PipeWire source");
            drop(state); // This will clean up the PipeWire stream
        }
        Ok(())
    }

    fn is_seekable(&self) -> bool {
        false
    }

    fn caps(&self, filter: Option<&gst::Caps>) -> Option<gst::Caps> {
        let state_guard = self.state.lock();
        let mut caps = if let Some(ref state) = *state_guard {
            match state.actual_output_mode {
                #[cfg(feature = "cuda")]
                OutputMode::Cuda => VideoCapsBuilder::new()
                    .features([CAPS_FEATURE_MEMORY_CUDA_MEMORY])
                    .format_list([VideoFormat::Bgra, VideoFormat::Rgba, VideoFormat::Nv12])
                    .build(),
                OutputMode::DmaBuf => VideoCapsBuilder::new()
                    .features([gstreamer_allocators::CAPS_FEATURE_MEMORY_DMABUF])
                    .format(VideoFormat::DmaDrm)
                    .build(),
                _ => VideoCapsBuilder::new()
                    .format_list([VideoFormat::Bgra, VideoFormat::Rgba])
                    .build(),
            }
        } else {
            // Not started yet, return all supported caps
            let mut all_caps = gst::Caps::new_empty();
            #[cfg(feature = "cuda")]
            {
                all_caps.merge(
                    VideoCapsBuilder::new()
                        .features([CAPS_FEATURE_MEMORY_CUDA_MEMORY])
                        .format_list([VideoFormat::Bgra, VideoFormat::Rgba, VideoFormat::Nv12])
                        .build(),
                );
            }
            all_caps.merge(
                VideoCapsBuilder::new()
                    .features([gstreamer_allocators::CAPS_FEATURE_MEMORY_DMABUF])
                    .format(VideoFormat::DmaDrm)
                    .build(),
            );
            all_caps.merge(
                VideoCapsBuilder::new()
                    .format_list([VideoFormat::Bgra, VideoFormat::Rgba])
                    .build(),
            );
            all_caps
        };

        if let Some(filter) = filter {
            caps = caps.intersect(filter);
        }

        Some(caps)
    }

    fn set_caps(&self, caps: &gst::Caps) -> Result<(), gst::LoggableError> {
        gst::info!(CAT, imp = self, "Setting caps: {:?}", caps);

        let video_info = VideoInfo::from_caps(caps)?;
        let mut state_guard = self.state.lock();
        if let Some(state) = state_guard.as_mut() {
            state.video_info = Some(video_info);
        }

        self.parent_set_caps(caps)
    }
}

impl PushSrcImpl for PipeWireZeroCopySrc {
    fn create(
        &self,
        _buffer: Option<&mut gst::BufferRef>,
    ) -> Result<CreateSuccess, gst::FlowError> {
        let mut state_guard = self.state.lock();
        let state = state_guard.as_mut().ok_or(gst::FlowError::Eos)?;

        let stream = state.stream.as_ref().ok_or_else(|| {
            gst::error!(CAT, imp = self, "No PipeWire stream");
            gst::FlowError::Error
        })?;

        // Wait for next frame
        let frame = match stream.recv_frame() {
            Ok(f) => f,
            Err(e) => {
                gst::error!(CAT, imp = self, "Failed to receive frame: {}", e);
                return Err(gst::FlowError::Error);
            }
        };

        // Convert frame to GStreamer buffer based on output mode
        let buffer = match frame {
            FrameData::DmaBuf(dmabuf) => {
                match state.actual_output_mode {
                    #[cfg(feature = "cuda")]
                    OutputMode::Cuda => {
                        // Convert DMA-BUF to CUDA buffer
                        let cuda_ctx = state.cuda_context.as_ref().ok_or_else(|| {
                            gst::error!(CAT, imp = self, "No CUDA context for CUDA mode");
                            gst::FlowError::Error
                        })?;

                        // Get EGL display
                        let egl_display = cuda::get_current_egl_display().map_err(|e| {
                            gst::error!(CAT, imp = self, "Failed to get EGL display: {}", e);
                            gst::FlowError::Error
                        })?;

                        // Convert to CUDA buffer
                        cuda::dmabuf_to_cuda_buffer(&dmabuf, cuda_ctx, egl_display).map_err(
                            |e| {
                                gst::error!(
                                    CAT,
                                    imp = self,
                                    "Failed to convert DMA-BUF to CUDA: {}",
                                    e
                                );
                                gst::FlowError::Error
                            },
                        )?
                    }
                    OutputMode::DmaBuf => {
                        // Pass through DMA-BUF
                        self.create_dmabuf_buffer(&dmabuf)?
                    }
                    _ => {
                        // Copy to system memory
                        self.copy_dmabuf_to_system(&dmabuf)?
                    }
                }
            }
            FrameData::Shm {
                data,
                width,
                height,
                stride,
                format: _,
            } => {
                // Create system memory buffer
                self.create_system_buffer(&data, width, height, stride)?
            }
        };

        state.frame_count += 1;

        Ok(CreateSuccess::NewBuffer(buffer))
    }
}

impl PipeWireZeroCopySrc {
    /// Create a GStreamer buffer from DMA-BUF (passthrough mode)
    fn create_dmabuf_buffer(&self, dmabuf: &crate::dmabuf::DmaBuf) -> Result<gst::Buffer, gst::FlowError> {
        // For DMA-BUF passthrough, we'd use GstDmaBufAllocator
        // For now, fall back to copy
        gst::warning!(CAT, imp = self, "DMA-BUF passthrough not yet implemented, falling back to copy");
        self.copy_dmabuf_to_system(dmabuf)
    }

    /// Copy DMA-BUF contents to system memory
    fn copy_dmabuf_to_system(&self, dmabuf: &crate::dmabuf::DmaBuf) -> Result<gst::Buffer, gst::FlowError> {
        use std::os::fd::AsRawFd;

        let size = (dmabuf.height * dmabuf.planes[0].stride) as usize;
        let mut data = vec![0u8; size];

        // mmap the DMA-BUF and copy
        unsafe {
            let fd = dmabuf.planes[0].fd.as_raw_fd();
            let ptr = libc::mmap(
                std::ptr::null_mut(),
                size,
                libc::PROT_READ,
                libc::MAP_SHARED,
                fd,
                0,
            );

            if ptr == libc::MAP_FAILED {
                gst::error!(CAT, imp = self, "Failed to mmap DMA-BUF");
                return Err(gst::FlowError::Error);
            }

            std::ptr::copy_nonoverlapping(ptr as *const u8, data.as_mut_ptr(), size);
            libc::munmap(ptr, size);
        }

        self.create_system_buffer(&data, dmabuf.width, dmabuf.height, dmabuf.planes[0].stride)
    }

    /// Create a system memory GStreamer buffer
    fn create_system_buffer(
        &self,
        data: &[u8],
        width: u32,
        height: u32,
        stride: u32,
    ) -> Result<gst::Buffer, gst::FlowError> {
        let mut buffer = gst::Buffer::with_size(data.len()).map_err(|_| {
            gst::error!(CAT, imp = self, "Failed to allocate buffer");
            gst::FlowError::Error
        })?;

        {
            let buffer_ref = buffer.get_mut().unwrap();
            let mut map = buffer_ref.map_writable().map_err(|_| {
                gst::error!(CAT, imp = self, "Failed to map buffer");
                gst::FlowError::Error
            })?;
            map.copy_from_slice(data);
        }

        // Add video meta
        {
            let buffer_ref = buffer.get_mut().unwrap();
            let format = VideoFormat::Bgra; // Assume BGRA for now
            let info = VideoInfo::builder(format, width, height)
                .build()
                .map_err(|_| gst::FlowError::Error)?;

            gst_video::VideoMeta::add_full(
                buffer_ref,
                gst_video::VideoFrameFlags::empty(),
                format,
                width,
                height,
                &[0],
                &[stride as i32],
            )
            .map_err(|_| {
                gst::error!(CAT, imp = self, "Failed to add video meta");
                gst::FlowError::Error
            })?;
        }

        Ok(buffer)
    }
}
