//! The verifier's own recomputation of the labels it will challenge.
//!
//! Block i of a session at byte `base` lies in global chunk
//! `(base / 32) / chunk_blocks + i / chunk_blocks` at rank `i % chunk_blocks`
//! (the placement contract of sink_proto.h; pose-label prints the same).  A
//! chunk costs its full depth-robust scaffold, about 2 s on one core at
//! chunk_blocks 2^18, so the challenged chunks are labelled once each, in
//! parallel, while the source is still generating, and the challenge phase
//! only compares.  Each thread uses the in-place labeler with its own scratch
//! (pose_graph_scratch_bytes_inplace: 80 MiB at 2^18).

use crate::ffi;
use crate::proto::{Label, Seed, LABEL_BYTES};
use anyhow::{bail, Result};
use std::collections::{BTreeMap, HashMap};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;

/// Global chunk index of session block `index`.
pub fn chunk_of(base: u64, chunk_blocks: u32, index: u64) -> u64 {
    (base / LABEL_BYTES as u64) / chunk_blocks as u64 + index / chunk_blocks as u64
}

/// The expected label of every block in `indices` of a session.  `done`
/// counts finished chunks for progress reporting.  Uses up to `threads`
/// threads.
pub fn expected(
    seed: &Seed,
    algo: u8,
    chunk_blocks: u32,
    base: u64,
    indices: &[u64],
    threads: usize,
    done: &AtomicUsize,
) -> Result<HashMap<u64, Label>> {
    let cb = chunk_blocks as u64;
    let mut by_chunk: BTreeMap<u64, Vec<u64>> = BTreeMap::new();
    for &i in indices {
        by_chunk.entry(chunk_of(base, chunk_blocks, i)).or_default().push(i);
    }
    let chunks: Vec<(u64, Vec<u64>)> = by_chunk.into_iter().collect();
    let next = AtomicUsize::new(0);
    let out = Mutex::new(HashMap::with_capacity(indices.len()));
    let failed = Mutex::new(None);
    let threads = threads.clamp(1, chunks.len().max(1));

    std::thread::scope(|s| {
        for _ in 0..threads {
            s.spawn(|| {
                let mut scratch = vec![0u8; unsafe { ffi::pose_graph_scratch_bytes_inplace(chunk_blocks) }];
                let mut labels = vec![0u8; cb as usize * LABEL_BYTES];
                loop {
                    let k = next.fetch_add(1, Ordering::Relaxed);
                    let Some((chunk, idxs)) = chunks.get(k) else { break };
                    let rc = unsafe {
                        ffi::pose_graph_label_chunk_inplace(
                            seed.as_ptr(),
                            seed.len(),
                            *chunk,
                            labels.as_mut_ptr(),
                            scratch.as_mut_ptr(),
                            algo as i32,
                            chunk_blocks,
                        )
                    };
                    if rc != 0 {
                        *failed.lock().unwrap() = Some(*chunk);
                        break;
                    }
                    let mut o = out.lock().unwrap();
                    for &i in idxs {
                        let r = (i % cb) as usize * LABEL_BYTES;
                        o.insert(i, labels[r..r + LABEL_BYTES].try_into().unwrap());
                    }
                    drop(o);
                    done.fetch_add(1, Ordering::Relaxed);
                }
            });
        }
    });
    if let Some(chunk) = failed.into_inner().unwrap() {
        bail!("labelling chunk {chunk} failed (chunk_blocks {chunk_blocks}, algo {algo})");
    }
    Ok(out.into_inner().unwrap())
}

/// Number of distinct chunks `expected` will label for these indices.
pub fn chunk_count(base: u64, chunk_blocks: u32, indices: &[u64]) -> usize {
    let mut v: Vec<u64> = indices.iter().map(|&i| chunk_of(base, chunk_blocks, i)).collect();
    v.sort_unstable();
    v.dedup();
    v.len()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::raw::c_int;

    extern "C" {
        /// The region labeler pose-label and the old verifier use.
        fn pose_graph_label_region(
            seed: *const u8,
            seed_len: usize,
            region: *mut u8,
            region_len: usize,
            region_block_offset: u64,
            algo: c_int,
            chunk_blocks: u32,
        ) -> c_int;
    }

    /// Labels of the whole session [base, base + chunks*cb*32) as the region
    /// labeler stores them, chunk by chunk as pose-label does.
    fn reference(seed: &Seed, algo: u8, cb: u32, base: u64, chunks: u64) -> Vec<u8> {
        let chunk_bytes = cb as usize * LABEL_BYTES;
        let mut all = vec![0u8; chunk_bytes * chunks as usize];
        for j in 0..chunks {
            let chunk = chunk_of(base, cb, j * cb as u64);
            let rc = unsafe {
                pose_graph_label_region(
                    seed.as_ptr(),
                    seed.len(),
                    all[j as usize * chunk_bytes..].as_mut_ptr(),
                    chunk_bytes,
                    chunk * cb as u64,
                    algo as c_int,
                    cb,
                )
            };
            assert_eq!(rc, 0);
        }
        all
    }

    #[test]
    fn matches_region_labeler() {
        let seed: Seed = core::array::from_fn(|i| i as u8 * 7 + 1);
        // A chunk-aligned base, and one that is page- but not chunk-aligned
        // (the sink's 73 MiB carve-out on the dev VM starts at 0x337662000).
        for &(base, algo) in &[(0x2c000_0000u64, 0u8), (0x3_3766_2000, 1), (0, 2)] {
            let cb = 1024u32;
            let want = reference(&seed, algo, cb, base, 3);
            let idx: Vec<u64> = (0..3 * cb as u64).step_by(97).chain([3 * cb as u64 - 1]).collect();
            let done = AtomicUsize::new(0);
            let got = expected(&seed, algo, cb, base, &idx, 4, &done).unwrap();
            assert_eq!(done.load(Ordering::Relaxed), 3);
            for i in idx {
                let r = i as usize * LABEL_BYTES;
                assert_eq!(got[&i][..], want[r..r + LABEL_BYTES], "base {base:#x} algo {algo} block {i}");
            }
        }
    }

    #[test]
    fn chunk_numbering() {
        assert_eq!(chunk_of(0, 1024, 0), 0);
        assert_eq!(chunk_of(0, 1024, 1024), 1);
        assert_eq!(chunk_of(0x2c000_0000, 1 << 18, 0), 0x2c000_0000 / 32 / (1 << 18));
        assert_eq!(chunk_count(0, 1024, &[0, 5, 1023, 1024, 4096]), 3);
    }
}
