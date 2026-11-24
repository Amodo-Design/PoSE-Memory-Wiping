//! The few pose-common entry points the verifier calls (pose-common/include/
//! graph.h, hash.h, gpu_session.h), declared by hand: they are plain C with fixed-width
//! arguments, and the cross-check tests in labels.rs pin their behaviour
//! against the daemons' labeler.

use std::os::raw::c_int;

/// POSE_GRAPH_MAX_M: the largest chunk_blocks the graph supports.
pub const POSE_GRAPH_MAX_CHUNK_BLOCKS: u32 = 1 << 20;

extern "C" {
    /// n such that 2^(n+1) >= m; gamma = 2^n for a chunk of m blocks.
    pub fn pose_graph_parameter_n(m: u64) -> c_int;

    /// Per-thread scratch for pose_graph_label_chunk_inplace.
    pub fn pose_graph_scratch_bytes_inplace(chunk_blocks: u32) -> usize;

    /// Label chunk `chunk_index` of a session in place: out gets
    /// chunk_blocks labels, rank r at out[r].  Thread-safe with private
    /// scratch.  0 on success.
    pub fn pose_graph_label_chunk_inplace(
        session_seed: *const u8,
        seed_len: usize,
        chunk_index: u64,
        out: *mut u8,
        scratch: *mut u8,
        algo: c_int,
        chunk_blocks: u32,
    ) -> c_int;

    /// Bytes the GPU labeler allocates on the device besides the session
    /// buffer, keeping `inflight` super-chunks in flight (plain C in
    /// pose-common, the same figure the source-daemon checks against).
    pub fn pose_gpu_scratch_bytes(chunk_blocks: u32, inflight: u32) -> u64;
}

/// gamma = 2^parameter_n(chunk_blocks): the sequential depth a relay must
/// chain to fake one label of a chunk.
pub fn gamma(chunk_blocks: u32) -> u64 {
    1u64 << unsafe { pose_graph_parameter_n(chunk_blocks as u64) }
}
