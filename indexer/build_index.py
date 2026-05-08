#!/usr/bin/env python3
"""
Builds an IVF1 index from references.json.gz.

Output: single binary file `index.bin` with layout:

    [4]   magic "IVF1"
    [4]   n          (u32) original vector count
    [4]   k          (u32) number of clusters
    [4]   d          (u32) dimensions (14)
    [4]   total_blocks (u32) total 8-vector blocks
    [k*d*4]            centroids       (f32, row-major: k x d)
    [(k+1)*4]          block_offsets   (u32, per cluster, +1 sentinel)
    [total_blocks*8]   labels          (u8, padded with 0)
    [total_blocks*14*8*2] blocks       (i16, dim-major: each block = d x 8)

Quantization: i16(v) = round(v * 10000), clamped to int16 range.
Padding: each cluster's vectors padded to multiple of 8. Padded slots get
label=0 and quantized vector = 32767 (very far from any real query in q-space).
"""
import gzip
import json
import os
import struct
import sys
import time

import numpy as np
from sklearn.cluster import MiniBatchKMeans

QUANT_SCALE = 10000.0
PAD_SENTINEL = 0       # safe value (no int32 overflow during squared-distance accumulation)
PAD_LABEL = 255        # sentinel label for padding slots (skipped at search time)
DIMS = 14


def quantize(vecs: np.ndarray) -> np.ndarray:
    q = np.rint(vecs * QUANT_SCALE)
    q = np.clip(q, -32768, 32767)
    return q.astype(np.int16)


def main():
    input_path = sys.argv[1] if len(sys.argv) > 1 else 'resources/references.json.gz'
    output_dir = sys.argv[2] if len(sys.argv) > 2 else 'data'
    n_clusters = int(sys.argv[3]) if len(sys.argv) > 3 else 4096

    os.makedirs(output_dir, exist_ok=True)

    print(f"Loading {input_path}...")
    t0 = time.time()
    if input_path.endswith('.gz'):
        with gzip.open(input_path, 'rt', encoding='utf-8') as f:
            data = json.load(f)
    else:
        with open(input_path, 'r') as f:
            data = json.load(f)
    print(f"  Loaded {len(data)} records in {time.time()-t0:.1f}s")

    vectors = np.array([d['vector'] for d in data], dtype=np.float32)
    labels = np.array([1 if d['label'] == 'fraud' else 0 for d in data], dtype=np.uint8)
    n_vectors, n_dims = vectors.shape
    assert n_dims == DIMS, f"expected {DIMS} dims, got {n_dims}"
    print(f"  Shape: {vectors.shape}, fraud_rate={labels.mean()*100:.1f}%")

    print(f"MiniBatchKMeans k={n_clusters}...")
    t0 = time.time()
    km = MiniBatchKMeans(
        n_clusters=n_clusters,
        batch_size=10000,
        n_init=3,
        max_iter=100,
        random_state=42,
    )
    assignments = km.fit_predict(vectors)
    centroids = km.cluster_centers_.astype(np.float32)
    print(f"  Done in {time.time()-t0:.1f}s")

    print("Sorting by cluster...")
    order = np.argsort(assignments, kind='stable')
    sorted_vectors = vectors[order]
    sorted_labels = labels[order]
    sorted_assignments = assignments[order]

    qvecs = quantize(sorted_vectors)  # (n, 14) int16

    # Per-cluster grouping with padding to multiple of 8
    print("Grouping into 8-vector blocks...")
    block_offsets = np.zeros(n_clusters + 1, dtype=np.uint32)
    blocks_list = []   # each entry: (14,8) int16
    labels_list = []   # each entry: (8,) uint8

    cur = 0
    for c in range(n_clusters):
        # find run of cluster c in sorted array (binary search)
        start = np.searchsorted(sorted_assignments, c, side='left')
        end = np.searchsorted(sorted_assignments, c, side='right')
        cluster_vecs = qvecs[start:end]      # (m, 14)
        cluster_labs = sorted_labels[start:end]  # (m,)
        m = end - start
        n_blocks = (m + 7) // 8

        block_offsets[c] = cur
        cur += n_blocks

        if m == 0:
            continue
        # pad
        padded_n = n_blocks * 8
        if padded_n > m:
            pad = padded_n - m
            pad_v = np.full((pad, DIMS), PAD_SENTINEL, dtype=np.int16)
            cluster_vecs = np.concatenate([cluster_vecs, pad_v], axis=0)
            pad_l = np.full(pad, PAD_LABEL, dtype=np.uint8)
            cluster_labs = np.concatenate([cluster_labs, pad_l], axis=0)

        # reshape to (n_blocks, 8, 14) then transpose dims to (n_blocks, 14, 8)
        bv = cluster_vecs.reshape(n_blocks, 8, DIMS).transpose(0, 2, 1).copy()
        blocks_list.append(bv)
        labels_list.append(cluster_labs)

    block_offsets[n_clusters] = cur
    total_blocks = cur

    blocks_arr = np.concatenate(blocks_list, axis=0).astype(np.int16)
    labels_arr = np.concatenate(labels_list, axis=0).astype(np.uint8)
    print(f"  total_blocks={total_blocks}, padded_n={total_blocks*8}")

    # Write single index.bin
    out_path = os.path.join(output_dir, 'index.bin')
    print(f"Writing {out_path}...")
    with open(out_path, 'wb') as f:
        f.write(b'IVF1')
        f.write(struct.pack('<IIII', n_vectors, n_clusters, DIMS, total_blocks))
        f.write(centroids.tobytes())
        f.write(block_offsets.tobytes())
        f.write(labels_arr.tobytes())
        f.write(blocks_arr.tobytes())

    sz = os.path.getsize(out_path)
    print(f"\nindex.bin size: {sz/1024/1024:.1f} MB")
    print(f"  centroids: {centroids.nbytes/1024/1024:.2f} MB")
    print(f"  offsets:   {block_offsets.nbytes/1024:.1f} KB")
    print(f"  labels:    {labels_arr.nbytes/1024/1024:.2f} MB")
    print(f"  blocks:    {blocks_arr.nbytes/1024/1024:.2f} MB")
    print("Done!")


if __name__ == '__main__':
    main()
