//! PipeWire ScreenCast source element implementation
//!
//! This element captures video from PipeWire ScreenCast and outputs CUDA buffers.
//! It reuses the proven DMA-BUF → EGLImage → CUDA conversion from gst-wayland-display.

use gst::glib;
use gst::prelude::*;
use gst::subclass::prelude::*;
use gst_base::prelude::BaseSrcExt;
use gst_base::subclass::base_src::CreateSuccess;
use gst_base::subclass::prelude::*;
use gst_video::{VideoCapsBuilder, VideoFormat, VideoInfo};
use once_cell::sync::Lazy;
use std::sync::{Arc, Mutex};

#[cfg(feature = "cuda")]
use waylanddisplaycore::utils::allocator::cuda::{
    CUDABufferPool, CUDAContext, CAPS_FEATURE_MEMORY_CUDA_MEMORY,
};

/// Logging category for this element
static CAT: Lazy<gst::DebugCategory> = Lazy::new(|| {
    gst::DebugCategory::new(
        "pipewirezerocopysrc",
        gst::DebugColorFlags::empty(),
        Some("PipeWire zero-copy source"),
    )
});

/// Element settings (configured via properties)
#[derive(Debug, Default)]
pub struct Settings {
    /// PipeWire node ID to connect to (from ScreenCast portal)
    pipewire_node_id: Option<u32>,
    /// DRM render node for GPU operations
    render_node: Option<String>,
    #[cfg(feature = "cuda")]
    /// CUDA context for GPU buffer management
    cuda_context: Option<Arc<Mutex<CUDAContext>>>,
}

/// Element runtime state
pub struct State {
    /// PipeWire main loop (runs in separate thread)
    // TODO: Add PipeWire mainloop
    /// Current video info
    video_info: Option<VideoInfo>,
}

impl Default for State {
    fn default() -> Self {
        Self {
            video_info: None,
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
                    .construct()
                    .build(),
                #[cfg(feature = "cuda")]
                glib::ParamSpecInt::builder("cuda-device-id")
                    .nick("CUDA Device ID")
                    .blurb("CUDA device ID to use (-1 for auto)")
                    .default_value(-1)
                    .construct()
                    .build(),
            ]
        });

        PROPERTIES.as_ref()
    }

    fn set_property(&self, _id: usize, value: &glib::Value, pspec: &glib::ParamSpec) {
        match pspec.name() {
            "pipewire-node-id" => {
                let mut settings = self.settings.lock().unwrap();
                settings.pipewire_node_id = Some(value.get().expect("Type checked upstream"));
            }
            "render-node" => {
                let mut settings = self.settings.lock().unwrap();
                settings.render_node = value.get().expect("Type checked upstream");
            }
            #[cfg(feature = "cuda")]
            "cuda-device-id" => {
                let device_id: i32 = value.get().unwrap();
                if device_id >= 0 {
                    match CUDAContext::new(device_id) {
                        Ok(ctx) => {
                            let mut settings = self.settings.lock().unwrap();
                            settings.cuda_context = Some(Arc::new(Mutex::new(ctx)));
                        }
                        Err(e) => {
                            gst::warning!(CAT, "Failed to create CUDA context: {}", e);
                        }
                    }
                }
            }
            _ => unreachable!(),
        }
    }

    fn property(&self, _id: usize, pspec: &glib::ParamSpec) -> glib::Value {
        match pspec.name() {
            "pipewire-node-id" => {
                let settings = self.settings.lock().unwrap();
                settings.pipewire_node_id.unwrap_or(0).to_value()
            }
            "render-node" => {
                let settings = self.settings.lock().unwrap();
                settings
                    .render_node
                    .clone()
                    .unwrap_or_else(|| String::from("/dev/dri/renderD128"))
                    .to_value()
            }
            #[cfg(feature = "cuda")]
            "cuda-device-id" => {
                let settings = self.settings.lock().unwrap();
                match settings.cuda_context {
                    Some(_) => 0i32.to_value(),
                    None => (-1i32).to_value(),
                }
            }
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
                "Captures PipeWire ScreenCast with zero-copy CUDA output",
                "Wolf Project <https://github.com/games-on-whales/wolf>",
            )
        });

        Some(&*ELEMENT_METADATA)
    }

    fn pad_templates() -> &'static [gst::PadTemplate] {
        static PAD_TEMPLATES: Lazy<Vec<gst::PadTemplate>> = Lazy::new(|| {
            // System memory output (fallback)
            let mut caps = VideoCapsBuilder::new()
                .format(VideoFormat::Bgra)
                .height_range(..i32::MAX)
                .width_range(..i32::MAX)
                .framerate_range(gst::Fraction::new(1, 1)..gst::Fraction::new(i32::MAX, 1))
                .build();

            // DMA-BUF output (AMD/Intel)
            let dmabuf_caps = VideoCapsBuilder::new()
                .features([gstreamer_allocators::CAPS_FEATURE_MEMORY_DMABUF])
                .format(VideoFormat::DmaDrm)
                .height_range(..i32::MAX)
                .width_range(..i32::MAX)
                .framerate_range(gst::Fraction::new(1, 1)..gst::Fraction::new(i32::MAX, 1))
                .build();
            caps.merge(dmabuf_caps);

            // CUDA output (NVIDIA)
            #[cfg(feature = "cuda")]
            {
                let cuda_caps = VideoCapsBuilder::new()
                    .features([CAPS_FEATURE_MEMORY_CUDA_MEMORY])
                    .format_list([VideoFormat::Bgra, VideoFormat::Rgba])
                    .height_range(..i32::MAX)
                    .width_range(..i32::MAX)
                    .framerate_range(gst::Fraction::new(1, 1)..gst::Fraction::new(i32::MAX, 1))
                    .build();
                caps.merge(cuda_caps);
            }

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
        let mut state = self.state.lock().unwrap();
        if state.is_some() {
            return Ok(());
        }

        let settings = self.settings.lock().unwrap();
        let node_id = settings.pipewire_node_id.ok_or_else(|| {
            gst::error_msg!(
                gst::LibraryError::Settings,
                ("pipewire-node-id property must be set")
            )
        })?;

        gst::info!(CAT, "Starting PipeWire source for node {}", node_id);

        // TODO: Initialize PipeWire connection
        // 1. Create PipeWire MainLoop
        // 2. Create Stream connected to node_id
        // 3. Negotiate DMA-BUF format
        // 4. Start receiving frames

        *state = Some(State::default());

        Ok(())
    }

    fn stop(&self) -> Result<(), gst::ErrorMessage> {
        let mut state = self.state.lock().unwrap();
        if let Some(_state) = state.take() {
            gst::info!(CAT, "Stopping PipeWire source");
            // TODO: Cleanup PipeWire connection
        }
        Ok(())
    }

    fn is_seekable(&self) -> bool {
        false
    }

    fn caps(&self, filter: Option<&gst::Caps>) -> Option<gst::Caps> {
        let mut caps = VideoCapsBuilder::new()
            .format(VideoFormat::Bgra)
            .height_range(..i32::MAX)
            .width_range(..i32::MAX)
            .framerate_range(gst::Fraction::new(1, 1)..gst::Fraction::new(i32::MAX, 1))
            .build();

        #[cfg(feature = "cuda")]
        {
            let cuda_caps = VideoCapsBuilder::new()
                .features([CAPS_FEATURE_MEMORY_CUDA_MEMORY])
                .format_list([VideoFormat::Bgra, VideoFormat::Rgba])
                .height_range(..i32::MAX)
                .width_range(..i32::MAX)
                .framerate_range(gst::Fraction::new(1, 1)..gst::Fraction::new(i32::MAX, 1))
                .build();
            caps.merge(cuda_caps);
        }

        if let Some(filter) = filter {
            caps = caps.intersect(filter);
        }

        Some(caps)
    }

    fn set_caps(&self, caps: &gst::Caps) -> Result<(), gst::LoggableError> {
        gst::info!(CAT, "Setting caps: {:?}", caps);

        let video_info = VideoInfo::from_caps(caps)?;
        let mut state_guard = self.state.lock().unwrap();
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
        let mut state_guard = self.state.lock().unwrap();
        let Some(_state) = state_guard.as_mut() else {
            return Err(gst::FlowError::Eos);
        };

        // TODO: Implement frame capture
        // 1. Wait for PipeWire frame callback
        // 2. Extract DMA-BUF from SPA buffer
        // 3. Convert to CUDA using wayland-display-core:
        //    - EGLImage::from(&dmabuf, &egl_display)
        //    - CUDAImage::from(&egl_image, &cuda_context)
        // 4. Wrap in GstBuffer and return

        // For now, return error to indicate not implemented
        gst::error!(CAT, "Frame capture not yet implemented");
        Err(gst::FlowError::Error)
    }
}
