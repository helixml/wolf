//! GStreamer PipeWire source with zero-copy GPU buffer handling
//!
//! This crate provides a GStreamer element that captures video from PipeWire ScreenCast
//! and converts DMA-BUF frames directly to CUDA buffers, reusing the proven conversion
//! code from gst-wayland-display.
//!
//! ## Architecture
//!
//! ```text
//! PipeWire ScreenCast → DMA-BUF → EGLImage → CUDA Buffer → GStreamer
//! ```
//!
//! The key insight is that both gst-wayland-display and PipeWire ScreenCast output
//! DMA-BUFs. The same conversion path (DMA-BUF → EGLImage → CUDA) applies to both.
//!
//! ## Output Modes
//!
//! The element supports three output modes (auto-detected based on hardware):
//!
//! 1. **CUDA** (NVIDIA): DMA-BUF → EGLImage → CUDA buffer
//! 2. **DMABuf** (AMD/Intel): Pass-through DMA-BUF for VA-API encoding
//! 3. **System Memory** (Fallback): Copy to CPU memory

use gst::glib;

mod dmabuf;
mod cuda;
mod pipewire_stream;
mod pipewiresrc;

/// GStreamer plugin initialization
fn plugin_init(plugin: &gst::Plugin) -> Result<(), glib::BoolError> {
    pipewiresrc::register(plugin)?;
    Ok(())
}

gst::plugin_define!(
    pipewirezerocopysrc,
    env!("CARGO_PKG_DESCRIPTION"),
    plugin_init,
    concat!(env!("CARGO_PKG_VERSION"), "-", env!("COMMIT_ID")),
    "MIT",
    env!("CARGO_PKG_NAME"),
    env!("CARGO_PKG_NAME"),
    env!("CARGO_PKG_REPOSITORY"),
    env!("BUILD_REL_DATE")
);
