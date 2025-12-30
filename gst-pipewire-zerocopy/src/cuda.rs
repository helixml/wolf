//! CUDA and EGL interop for DMA-BUF → CUDA buffer conversion
//!
//! This module implements the DMA-BUF → EGLImage → CUDA conversion pipeline,
//! adapted from gst-wayland-display's wayland-display-core.

use crate::dmabuf::DmaBuf;
use gst::glib::ffi as glib_ffi;
use gst::glib::translate::ToGlibPtr;
use gst_video::{VideoInfo, VideoMeta};
use libloading::{Library, Symbol};
use once_cell::sync::OnceCell;
use std::ffi::c_void;
use std::os::fd::AsRawFd;
use std::os::raw::{c_char, c_int, c_uint};
use std::ptr;
use std::sync::Arc;

// =============================================================================
// Type Definitions
// =============================================================================

pub type GstCudaContext = *mut c_void;
pub type GstCudaStream = *mut c_void;
pub type GstBufferPool = *mut c_void;
pub type EGLDisplay = *mut c_void;
pub type EGLImageKHR = *mut c_void;
pub type EGLint = i32;
pub type CUcontext = *mut c_void;
pub type CUstream = *mut c_void;
pub type CUdeviceptr = u64;
pub type CUarray = *mut c_void;
pub type CUgraphicsResource = *mut c_void;
pub type CUresult = c_uint;

// =============================================================================
// Constants
// =============================================================================

pub const CUDA_SUCCESS: CUresult = 0;
pub const EGL_NO_IMAGE_KHR: EGLImageKHR = ptr::null_mut();
pub const EGL_LINUX_DMA_BUF_EXT: u32 = 0x3270;
pub const EGL_WIDTH: EGLint = 0x3057;
pub const EGL_HEIGHT: EGLint = 0x3056;
pub const EGL_LINUX_DRM_FOURCC_EXT: EGLint = 0x3271;
pub const EGL_NONE: EGLint = 0x3038;

pub const EGL_DMA_BUF_PLANE0_FD_EXT: EGLint = 0x3272;
pub const EGL_DMA_BUF_PLANE0_OFFSET_EXT: EGLint = 0x3273;
pub const EGL_DMA_BUF_PLANE0_PITCH_EXT: EGLint = 0x3274;
pub const EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT: EGLint = 0x3443;
pub const EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT: EGLint = 0x3444;

pub const EGL_DMA_BUF_PLANE1_FD_EXT: EGLint = 0x3275;
pub const EGL_DMA_BUF_PLANE1_OFFSET_EXT: EGLint = 0x3276;
pub const EGL_DMA_BUF_PLANE1_PITCH_EXT: EGLint = 0x3277;
pub const EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT: EGLint = 0x3445;
pub const EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT: EGLint = 0x3446;

pub const EGL_DMA_BUF_PLANE2_FD_EXT: EGLint = 0x3278;
pub const EGL_DMA_BUF_PLANE2_OFFSET_EXT: EGLint = 0x3279;
pub const EGL_DMA_BUF_PLANE2_PITCH_EXT: EGLint = 0x327A;
pub const EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT: EGLint = 0x3447;
pub const EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT: EGLint = 0x3448;

pub const CAPS_FEATURE_MEMORY_CUDA_MEMORY: &str = "memory:CUDAMemory";

// =============================================================================
// CUDA Memcpy Structures
// =============================================================================

const MAX_PLANES: usize = 3;

#[repr(C)]
pub struct CUeglFrame {
    pub frame: CUeglFrameUnion,
    pub width: c_uint,
    pub height: c_uint,
    pub depth: c_uint,
    pub pitch: c_uint,
    pub plane_count: c_uint,
    pub num_channels: c_uint,
    pub frame_type: c_uint,
    pub egl_color_format: c_uint,
    pub cu_format: c_uint,
}

#[repr(C)]
pub union CUeglFrameUnion {
    pub p_array: [CUarray; MAX_PLANES],
    pub p_pitch: [*mut c_void; MAX_PLANES],
}

#[repr(C)]
pub struct CUDA_MEMCPY2D {
    pub src_x_in_bytes: usize,
    pub src_y: usize,
    pub src_memory_type: c_uint,
    pub src_host: *const c_void,
    pub src_device: CUdeviceptr,
    pub src_array: CUarray,
    pub src_pitch: usize,
    pub dst_x_in_bytes: usize,
    pub dst_y: usize,
    pub dst_memory_type: c_uint,
    pub dst_host: *mut c_void,
    pub dst_device: CUdeviceptr,
    pub dst_array: CUarray,
    pub dst_pitch: usize,
    pub width_in_bytes: usize,
    pub height: usize,
}

// Memory types
pub const CU_MEMORYTYPE_HOST: c_uint = 0x01;
pub const CU_MEMORYTYPE_DEVICE: c_uint = 0x02;
pub const CU_MEMORYTYPE_ARRAY: c_uint = 0x03;

// =============================================================================
// Function Pointer Types
// =============================================================================

type PFN_eglGetProcAddress = unsafe extern "C" fn(*const c_char) -> *mut c_void;
type PFN_eglCreateImageKHR = unsafe extern "C" fn(
    EGLDisplay,
    *mut c_void,  // EGLContext
    u32,          // target
    *mut c_void,  // buffer
    *const EGLint,
) -> EGLImageKHR;
type PFN_eglDestroyImageKHR = unsafe extern "C" fn(EGLDisplay, EGLImageKHR) -> c_uint;
type PFN_eglGetCurrentDisplay = unsafe extern "C" fn() -> EGLDisplay;

type CuGraphicsEGLRegisterImageFn = unsafe extern "C" fn(
    *mut CUgraphicsResource,
    EGLImageKHR,
    c_uint,
) -> CUresult;
type CuGraphicsUnregisterResourceFn = unsafe extern "C" fn(CUgraphicsResource) -> CUresult;
type CuGraphicsResourceGetMappedEglFrameFn = unsafe extern "C" fn(
    *mut CUeglFrame,
    CUgraphicsResource,
    c_uint,
    c_uint,
) -> CUresult;
type CuCtxPushCurrentFn = unsafe extern "C" fn(CUcontext) -> CUresult;
type CuCtxPopCurrentFn = unsafe extern "C" fn(*mut CUcontext) -> CUresult;
type CuMemcpy2DAsyncFn = unsafe extern "C" fn(*const CUDA_MEMCPY2D, CUstream) -> CUresult;
type CuStreamSynchronizeFn = unsafe extern "C" fn(CUstream) -> CUresult;
type CuMemAllocPitchFn = unsafe extern "C" fn(*mut CUdeviceptr, *mut usize, usize, usize, c_uint) -> CUresult;
type CuMemFreeFn = unsafe extern "C" fn(CUdeviceptr) -> CUresult;

// =============================================================================
// GStreamer CUDA FFI (from libgstcuda)
// =============================================================================

#[link(name = "gstcuda-1.0")]
unsafe extern "C" {
    fn gst_cuda_load_library() -> glib_ffi::gboolean;
    fn gst_cuda_memory_init_once();
    fn gst_cuda_context_new(device_id: c_int) -> GstCudaContext;
    fn gst_cuda_context_get_handle(ctx: GstCudaContext) -> CUcontext;
    fn gst_cuda_context_push(ctx: GstCudaContext) -> glib_ffi::gboolean;
    fn gst_cuda_context_pop(old_ctx: *mut CUcontext) -> glib_ffi::gboolean;
    fn gst_cuda_stream_new(ctx: GstCudaContext) -> GstCudaStream;
    fn gst_cuda_stream_get_handle(stream: GstCudaStream) -> CUstream;
    fn gst_cuda_stream_unref(stream: GstCudaStream);
    fn gst_cuda_buffer_pool_new(ctx: GstCudaContext) -> GstBufferPool;
}

// =============================================================================
// Library Handles
// =============================================================================

struct EglFunctions {
    _lib: Library,
    get_proc_address: PFN_eglGetProcAddress,
    create_image: PFN_eglCreateImageKHR,
    destroy_image: PFN_eglDestroyImageKHR,
    get_current_display: PFN_eglGetCurrentDisplay,
}

struct CudaFunctions {
    _lib: Library,
    register_image: CuGraphicsEGLRegisterImageFn,
    unregister_resource: CuGraphicsUnregisterResourceFn,
    get_mapped_frame: CuGraphicsResourceGetMappedEglFrameFn,
    ctx_push: CuCtxPushCurrentFn,
    ctx_pop: CuCtxPopCurrentFn,
    memcpy_2d_async: CuMemcpy2DAsyncFn,
    stream_sync: CuStreamSynchronizeFn,
    mem_alloc_pitch: CuMemAllocPitchFn,
    mem_free: CuMemFreeFn,
}

static EGL_FUNCTIONS: OnceCell<EglFunctions> = OnceCell::new();
static CUDA_FUNCTIONS: OnceCell<CudaFunctions> = OnceCell::new();
static CUDA_INITIALIZED: OnceCell<bool> = OnceCell::new();

fn load_egl_functions() -> Result<&'static EglFunctions, String> {
    EGL_FUNCTIONS.get_or_try_init(|| {
        unsafe {
            let lib = Library::new("libEGL.so.1")
                .or_else(|_| Library::new("libEGL.so"))
                .map_err(|e| format!("Failed to load EGL library: {}", e))?;

            let get_proc_address: Symbol<PFN_eglGetProcAddress> = lib
                .get(b"eglGetProcAddress\0")
                .map_err(|e| format!("Failed to get eglGetProcAddress: {}", e))?;
            let get_proc_address = *get_proc_address;

            let get_current_display: Symbol<PFN_eglGetCurrentDisplay> = lib
                .get(b"eglGetCurrentDisplay\0")
                .map_err(|e| format!("Failed to get eglGetCurrentDisplay: {}", e))?;
            let get_current_display = *get_current_display;

            // Load extension functions via eglGetProcAddress
            let create_image_ptr = get_proc_address(b"eglCreateImageKHR\0".as_ptr() as *const c_char);
            let destroy_image_ptr = get_proc_address(b"eglDestroyImageKHR\0".as_ptr() as *const c_char);

            if create_image_ptr.is_null() || destroy_image_ptr.is_null() {
                return Err("EGL_KHR_image extension not available".to_string());
            }

            Ok(EglFunctions {
                _lib: lib,
                get_proc_address,
                create_image: std::mem::transmute(create_image_ptr),
                destroy_image: std::mem::transmute(destroy_image_ptr),
                get_current_display,
            })
        }
    })
}

fn load_cuda_functions() -> Result<&'static CudaFunctions, String> {
    CUDA_FUNCTIONS.get_or_try_init(|| {
        unsafe {
            let lib = Library::new("libcuda.so.1")
                .or_else(|_| Library::new("libcuda.so"))
                .map_err(|e| format!("Failed to load CUDA library: {}", e))?;

            macro_rules! load_sym {
                ($name:ident) => {{
                    let sym: Symbol<$name> = lib
                        .get(concat!(stringify!($name), "\0").as_bytes())
                        .map_err(|e| format!("Failed to get {}: {}", stringify!($name), e))?;
                    *sym
                }};
            }

            // Try cuGraphicsEGLRegisterImage (CUDA-EGL interop)
            let register_image: Symbol<CuGraphicsEGLRegisterImageFn> = lib
                .get(b"cuGraphicsEGLRegisterImage\0")
                .map_err(|e| format!("cuGraphicsEGLRegisterImage not found (CUDA-EGL not supported?): {}", e))?;
            let unregister_resource: Symbol<CuGraphicsUnregisterResourceFn> = lib
                .get(b"cuGraphicsUnregisterResource\0")
                .map_err(|e| format!("cuGraphicsUnregisterResource: {}", e))?;
            let get_mapped_frame: Symbol<CuGraphicsResourceGetMappedEglFrameFn> = lib
                .get(b"cuGraphicsResourceGetMappedEglFrame\0")
                .map_err(|e| format!("cuGraphicsResourceGetMappedEglFrame: {}", e))?;
            let ctx_push: Symbol<CuCtxPushCurrentFn> = lib
                .get(b"cuCtxPushCurrent_v2\0")
                .or_else(|_| lib.get(b"cuCtxPushCurrent\0"))
                .map_err(|e| format!("cuCtxPushCurrent: {}", e))?;
            let ctx_pop: Symbol<CuCtxPopCurrentFn> = lib
                .get(b"cuCtxPopCurrent_v2\0")
                .or_else(|_| lib.get(b"cuCtxPopCurrent\0"))
                .map_err(|e| format!("cuCtxPopCurrent: {}", e))?;
            let memcpy_2d_async: Symbol<CuMemcpy2DAsyncFn> = lib
                .get(b"cuMemcpy2DAsync_v2\0")
                .or_else(|_| lib.get(b"cuMemcpy2DAsync\0"))
                .map_err(|e| format!("cuMemcpy2DAsync: {}", e))?;
            let stream_sync: Symbol<CuStreamSynchronizeFn> = lib
                .get(b"cuStreamSynchronize\0")
                .map_err(|e| format!("cuStreamSynchronize: {}", e))?;
            let mem_alloc_pitch: Symbol<CuMemAllocPitchFn> = lib
                .get(b"cuMemAllocPitch_v2\0")
                .or_else(|_| lib.get(b"cuMemAllocPitch\0"))
                .map_err(|e| format!("cuMemAllocPitch: {}", e))?;
            let mem_free: Symbol<CuMemFreeFn> = lib
                .get(b"cuMemFree_v2\0")
                .or_else(|_| lib.get(b"cuMemFree\0"))
                .map_err(|e| format!("cuMemFree: {}", e))?;

            Ok(CudaFunctions {
                _lib: lib,
                register_image: *register_image,
                unregister_resource: *unregister_resource,
                get_mapped_frame: *get_mapped_frame,
                ctx_push: *ctx_push,
                ctx_pop: *ctx_pop,
                memcpy_2d_async: *memcpy_2d_async,
                stream_sync: *stream_sync,
                mem_alloc_pitch: *mem_alloc_pitch,
                mem_free: *mem_free,
            })
        }
    })
}

// =============================================================================
// Public API
// =============================================================================

/// Initialize CUDA/GStreamer integration
pub fn init_cuda() -> Result<(), String> {
    CUDA_INITIALIZED.get_or_try_init(|| {
        unsafe {
            if gst_cuda_load_library() == glib_ffi::GFALSE {
                return Err("Failed to load GStreamer CUDA library".to_string());
            }
            gst_cuda_memory_init_once();
        }
        load_egl_functions()?;
        load_cuda_functions()?;
        Ok(true)
    })?;
    Ok(())
}

/// Check if CUDA is available
pub fn is_cuda_available() -> bool {
    init_cuda().is_ok()
}

/// CUDA context wrapper
#[derive(Debug)]
pub struct CudaContext {
    gst_ctx: GstCudaContext,
    stream: Option<GstCudaStream>,
}

impl CudaContext {
    /// Create a new CUDA context for the given device
    pub fn new(device_id: i32) -> Result<Self, String> {
        init_cuda()?;

        let gst_ctx = unsafe { gst_cuda_context_new(device_id) };
        if gst_ctx.is_null() {
            return Err(format!("Failed to create CUDA context for device {}", device_id));
        }

        let stream = unsafe { gst_cuda_stream_new(gst_ctx) };

        Ok(CudaContext {
            gst_ctx,
            stream: if stream.is_null() { None } else { Some(stream) },
        })
    }

    /// Get the raw CUDA context handle
    pub fn handle(&self) -> CUcontext {
        unsafe { gst_cuda_context_get_handle(self.gst_ctx) }
    }

    /// Get the CUDA stream handle
    pub fn stream_handle(&self) -> Option<CUstream> {
        self.stream.map(|s| unsafe { gst_cuda_stream_get_handle(s) })
    }

    /// Push this context to make it current
    pub fn push(&self) -> Result<CudaContextGuard, String> {
        CudaContextGuard::new(self)
    }
}

impl Drop for CudaContext {
    fn drop(&mut self) {
        unsafe {
            if let Some(stream) = self.stream {
                gst_cuda_stream_unref(stream);
            }
            gst::ffi::gst_object_unref(self.gst_ctx as *mut gst::ffi::GstObject);
        }
    }
}

unsafe impl Send for CudaContext {}
unsafe impl Sync for CudaContext {}

/// RAII guard for CUDA context push/pop
pub struct CudaContextGuard {
    _marker: std::marker::PhantomData<*mut ()>,
}

impl CudaContextGuard {
    fn new(ctx: &CudaContext) -> Result<Self, String> {
        unsafe {
            if gst_cuda_context_push(ctx.gst_ctx) == glib_ffi::GFALSE {
                return Err("Failed to push CUDA context".to_string());
            }
        }
        Ok(CudaContextGuard { _marker: std::marker::PhantomData })
    }
}

impl Drop for CudaContextGuard {
    fn drop(&mut self) {
        unsafe {
            let mut old_ctx: CUcontext = ptr::null_mut();
            gst_cuda_context_pop(&mut old_ctx);
        }
    }
}

/// EGL image created from a DMA-BUF
pub struct EglImage {
    image: EGLImageKHR,
    display: EGLDisplay,
}

impl EglImage {
    /// Create an EGLImage from a DMA-BUF
    pub fn from_dmabuf(dmabuf: &DmaBuf, display: EGLDisplay) -> Result<Self, String> {
        let egl = load_egl_functions()?;

        let modifier_lo = (dmabuf.modifier & 0xFFFFFFFF) as EGLint;
        let modifier_hi = ((dmabuf.modifier >> 32) & 0xFFFFFFFF) as EGLint;

        let mut attribs = vec![
            EGL_WIDTH, dmabuf.width as EGLint,
            EGL_HEIGHT, dmabuf.height as EGLint,
            EGL_LINUX_DRM_FOURCC_EXT, dmabuf.fourcc as EGLint,
        ];

        // Add plane attributes
        for (idx, plane) in dmabuf.planes.iter().enumerate() {
            let fd = plane.as_raw_fd();
            match idx {
                0 => {
                    attribs.extend_from_slice(&[
                        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
                        EGL_DMA_BUF_PLANE0_OFFSET_EXT, plane.offset as EGLint,
                        EGL_DMA_BUF_PLANE0_PITCH_EXT, plane.stride as EGLint,
                        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, modifier_lo,
                        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, modifier_hi,
                    ]);
                }
                1 => {
                    attribs.extend_from_slice(&[
                        EGL_DMA_BUF_PLANE1_FD_EXT, fd,
                        EGL_DMA_BUF_PLANE1_OFFSET_EXT, plane.offset as EGLint,
                        EGL_DMA_BUF_PLANE1_PITCH_EXT, plane.stride as EGLint,
                        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, modifier_lo,
                        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, modifier_hi,
                    ]);
                }
                2 => {
                    attribs.extend_from_slice(&[
                        EGL_DMA_BUF_PLANE2_FD_EXT, fd,
                        EGL_DMA_BUF_PLANE2_OFFSET_EXT, plane.offset as EGLint,
                        EGL_DMA_BUF_PLANE2_PITCH_EXT, plane.stride as EGLint,
                        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, modifier_lo,
                        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, modifier_hi,
                    ]);
                }
                _ => {}
            }
        }
        attribs.push(EGL_NONE);

        let image = unsafe {
            (egl.create_image)(
                display,
                ptr::null_mut(), // EGL_NO_CONTEXT
                EGL_LINUX_DMA_BUF_EXT,
                ptr::null_mut(), // No buffer, attributes contain DMA-BUF info
                attribs.as_ptr(),
            )
        };

        if image == EGL_NO_IMAGE_KHR {
            return Err("Failed to create EGLImage from DMA-BUF".to_string());
        }

        Ok(EglImage { image, display })
    }

    /// Get the raw EGLImage handle
    pub fn handle(&self) -> EGLImageKHR {
        self.image
    }
}

impl Drop for EglImage {
    fn drop(&mut self) {
        if let Ok(egl) = load_egl_functions() {
            unsafe {
                (egl.destroy_image)(self.display, self.image);
            }
        }
    }
}

/// CUDA image registered from an EGLImage
pub struct CudaImage {
    #[allow(dead_code)]
    egl_image: EglImage,
    resource: CUgraphicsResource,
}

impl CudaImage {
    /// Register an EGLImage with CUDA
    pub fn from_egl_image(egl_image: EglImage, cuda_ctx: &CudaContext) -> Result<Self, String> {
        let cuda = load_cuda_functions()?;
        let _guard = cuda_ctx.push()?;

        let mut resource: CUgraphicsResource = ptr::null_mut();
        let result = unsafe {
            (cuda.register_image)(&mut resource, egl_image.image, 0)
        };

        if result != CUDA_SUCCESS {
            return Err(format!("Failed to register EGLImage with CUDA: error {}", result));
        }

        Ok(CudaImage { egl_image, resource })
    }

    /// Get the mapped EGL frame data
    pub fn get_egl_frame(&self, cuda_ctx: &CudaContext) -> Result<CUeglFrame, String> {
        let cuda = load_cuda_functions()?;
        let _guard = cuda_ctx.push()?;

        let mut frame = std::mem::MaybeUninit::<CUeglFrame>::uninit();
        let result = unsafe {
            (cuda.get_mapped_frame)(frame.as_mut_ptr(), self.resource, 0, 0)
        };

        if result != CUDA_SUCCESS {
            return Err(format!("Failed to get mapped EGL frame: error {}", result));
        }

        Ok(unsafe { frame.assume_init() })
    }
}

impl Drop for CudaImage {
    fn drop(&mut self) {
        if let Ok(cuda) = load_cuda_functions() {
            unsafe {
                (cuda.unregister_resource)(self.resource);
            }
        }
    }
}

/// Convert a DMA-BUF to a CUDA-backed GStreamer buffer
pub fn dmabuf_to_cuda_buffer(
    dmabuf: &DmaBuf,
    cuda_ctx: &CudaContext,
    egl_display: EGLDisplay,
) -> Result<gst::Buffer, String> {
    let cuda = load_cuda_functions()?;
    let _guard = cuda_ctx.push()?;

    // Create EGLImage from DMA-BUF
    let egl_image = EglImage::from_dmabuf(dmabuf, egl_display)?;

    // Register with CUDA
    let cuda_image = CudaImage::from_egl_image(egl_image, cuda_ctx)?;

    // Get the mapped frame
    let egl_frame = cuda_image.get_egl_frame(cuda_ctx)?;

    // Allocate CUDA memory and copy
    let width = egl_frame.width as usize;
    let height = egl_frame.height as usize;
    let bytes_per_pixel = 4; // Assuming BGRA/RGBA

    let mut dst_ptr: CUdeviceptr = 0;
    let mut dst_pitch: usize = 0;

    let result = unsafe {
        (cuda.mem_alloc_pitch)(
            &mut dst_ptr,
            &mut dst_pitch,
            width * bytes_per_pixel,
            height,
            16, // Element size for alignment
        )
    };

    if result != CUDA_SUCCESS {
        return Err(format!("Failed to allocate CUDA memory: error {}", result));
    }

    // Copy from EGL frame to our buffer
    let copy_params = CUDA_MEMCPY2D {
        src_x_in_bytes: 0,
        src_y: 0,
        src_memory_type: CU_MEMORYTYPE_DEVICE,
        src_host: ptr::null(),
        src_device: unsafe { egl_frame.frame.p_pitch[0] as CUdeviceptr },
        src_array: ptr::null_mut(),
        src_pitch: egl_frame.pitch as usize,
        dst_x_in_bytes: 0,
        dst_y: 0,
        dst_memory_type: CU_MEMORYTYPE_DEVICE,
        dst_host: ptr::null_mut(),
        dst_device: dst_ptr,
        dst_array: ptr::null_mut(),
        dst_pitch,
        width_in_bytes: width * bytes_per_pixel,
        height,
    };

    let stream = cuda_ctx.stream_handle().unwrap_or(ptr::null_mut());
    let result = unsafe { (cuda.memcpy_2d_async)(&copy_params, stream) };
    if result != CUDA_SUCCESS {
        unsafe { (cuda.mem_free)(dst_ptr) };
        return Err(format!("Failed to copy to CUDA buffer: error {}", result));
    }

    // Synchronize
    if !stream.is_null() {
        let result = unsafe { (cuda.stream_sync)(stream) };
        if result != CUDA_SUCCESS {
            unsafe { (cuda.mem_free)(dst_ptr) };
            return Err(format!("Failed to sync CUDA stream: error {}", result));
        }
    }

    // Create GStreamer buffer wrapping the CUDA memory
    // Note: In a real implementation, we'd use GstCudaBufferPool
    // For now, return an error as full integration requires more work
    Err("Full GStreamer CUDA buffer integration not yet implemented".to_string())
}

/// Get the current EGL display
pub fn get_current_egl_display() -> Result<EGLDisplay, String> {
    let egl = load_egl_functions()?;
    let display = unsafe { (egl.get_current_display)() };
    if display.is_null() {
        return Err("No current EGL display".to_string());
    }
    Ok(display)
}
