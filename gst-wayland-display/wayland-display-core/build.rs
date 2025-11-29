use pkg_config;
use std::env;

fn main() {
    // Check if the cuda feature is enabled
    if env::var("CARGO_FEATURE_CUDA").is_ok() {
        // Link GStreamer CUDA library
        if let Err(e) = pkg_config::Config::new()
            .atleast_version("1.24")
            .probe("gstreamer-cuda-1.0")
        {
            eprintln!(
                "Warning: gstreamer-cuda-1.0 not found via pkg-config: {}",
                e
            );
            eprintln!("Attempting to link manually...");

            // Fallback: try to link directly
            println!("cargo:rustc-link-lib=gstcuda-1.0");
        }
    }

    // Rerun if build script changes
    println!("cargo:rerun-if-changed=build.rs");
}
