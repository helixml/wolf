//! GStreamer PipeWire source with zero-copy GPU buffer handling
//!
//! Captures PipeWire ScreenCast DMA-BUFs and converts them to CUDA buffers
//! using the proven code from gst-wayland-display.

use gst::glib;

mod pipewire_stream;
mod pipewiresrc;

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
