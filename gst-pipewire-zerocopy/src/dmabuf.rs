//! DMA-BUF handling for PipeWire buffers
//!
//! This module provides a simple DMA-BUF representation that can be constructed
//! from PipeWire's SPA buffer format, independent of smithay.

use std::os::fd::{BorrowedFd, OwnedFd, FromRawFd, AsRawFd};

/// Maximum number of planes in a DMA-BUF
pub const MAX_PLANES: usize = 4;

/// A plane of a DMA-BUF
#[derive(Debug)]
pub struct DmaBufPlane {
    /// File descriptor for this plane
    pub fd: OwnedFd,
    /// Offset into the buffer
    pub offset: u32,
    /// Stride (bytes per row)
    pub stride: u32,
}

/// DMA-BUF representation for PipeWire buffers
#[derive(Debug)]
pub struct DmaBuf {
    /// Width in pixels
    pub width: u32,
    /// Height in pixels
    pub height: u32,
    /// DRM fourcc format code
    pub fourcc: u32,
    /// DRM modifier
    pub modifier: u64,
    /// Planes (1-4 depending on format)
    pub planes: Vec<DmaBufPlane>,
}

impl DmaBuf {
    /// Create a new DMA-BUF from raw parameters
    ///
    /// # Safety
    /// The file descriptors must be valid DMA-BUF fds that remain valid
    /// for the lifetime of this struct.
    pub unsafe fn from_raw(
        width: u32,
        height: u32,
        fourcc: u32,
        modifier: u64,
        fds: &[i32],
        offsets: &[u32],
        strides: &[u32],
    ) -> Result<Self, &'static str> {
        if fds.is_empty() || fds.len() > MAX_PLANES {
            return Err("Invalid plane count");
        }
        if fds.len() != offsets.len() || fds.len() != strides.len() {
            return Err("Mismatched plane data lengths");
        }

        let planes: Vec<DmaBufPlane> = fds
            .iter()
            .zip(offsets.iter())
            .zip(strides.iter())
            .map(|((&fd, &offset), &stride)| {
                // Duplicate the fd so we own it
                let new_fd = libc::dup(fd);
                if new_fd < 0 {
                    panic!("Failed to dup DMA-BUF fd");
                }
                DmaBufPlane {
                    fd: OwnedFd::from_raw_fd(new_fd),
                    offset,
                    stride,
                }
            })
            .collect();

        Ok(DmaBuf {
            width,
            height,
            fourcc,
            modifier,
            planes,
        })
    }

    /// Get the number of planes
    pub fn plane_count(&self) -> usize {
        self.planes.len()
    }

    /// Get a borrowed fd for a plane
    pub fn plane_fd(&self, index: usize) -> Option<BorrowedFd<'_>> {
        self.planes.get(index).map(|p| p.fd.as_fd())
    }
}

impl AsRawFd for DmaBufPlane {
    fn as_raw_fd(&self) -> i32 {
        self.fd.as_raw_fd()
    }
}

use std::os::fd::AsFd;

impl AsFd for DmaBufPlane {
    fn as_fd(&self) -> BorrowedFd<'_> {
        self.fd.as_fd()
    }
}
