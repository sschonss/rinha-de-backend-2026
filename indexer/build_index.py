#!/usr/bin/env python3
"""
Builds an IVF (Inverted File Index) from references.json.gz.

Reads 3M labeled vectors, clusters them via MiniBatchKMeans,
and outputs binary files for mmap-based loading at runtime.

Output files:
  - centroids.bin   : float32[n_clusters × 14] — cluster centroids (full precision)
  - vectors.bin     : uint8[n_vectors × 14]    — quantized vectors sorted by cluster
  - labels.bin      : uint8[n_vectors]          — labels sorted by cluster (0=legit, 1=fraud)
  - offsets.bin      : uint32[n_clusters × 2]   — (start_index, count) per cluster
  - meta.bin        : uint32[3]                 — (n_vectors, n_clusters, n_dims)
  - quant.bin       : float32[n_dims × 2]       — (min, scale) per dimension for dequantization
"""
import json
import gzip
import struct
import sys
import os
import time
import numpy as np
from sklearn.cluster import MiniBatchKMeans

def main():
    input_path = sys.argv[1] if len(sys.argv) > 1 else 'resources/references.json.gz'
    output_dir = sys.argv[2] if len(sys.argv) > 2 else 'data'
    n_clusters = int(sys.argv[3]) if len(sys.argv) > 3 else 1500

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

    print("Converting to numpy arrays...")
    vectors = np.array([d['vector'] for d in data], dtype=np.float32)
    labels = np.array([1 if d['label'] == 'fraud' else 0 for d in data], dtype=np.uint8)
    n_vectors, n_dims = vectors.shape
    print(f"  Shape: {vectors.shape}, labels: {labels.shape}")
    print(f"  Fraud rate: {labels.sum()}/{n_vectors} ({100*labels.mean():.1f}%)")

    print(f"Running MiniBatchKMeans with {n_clusters} clusters...")
    t0 = time.time()
    kmeans = MiniBatchKMeans(
        n_clusters=n_clusters,
        batch_size=10000,
        n_init=3,
        max_iter=100,
        random_state=42,
    )
    assignments = kmeans.fit_predict(vectors)
    centroids = kmeans.cluster_centers_.astype(np.float32)
    print(f"  Clustering done in {time.time()-t0:.1f}s")

    print("Sorting vectors by cluster assignment...")
    order = np.argsort(assignments)
    sorted_vectors = vectors[order]
    sorted_labels = labels[order]
    sorted_assignments = assignments[order]

    # Quantize vectors to uint8
    print("Quantizing vectors to uint8...")
    vmin = sorted_vectors.min(axis=0).astype(np.float32)
    vmax = sorted_vectors.max(axis=0).astype(np.float32)
    vrange = vmax - vmin
    vrange[vrange == 0] = 1.0  # avoid division by zero
    scale = 255.0 / vrange
    quantized = np.clip(((sorted_vectors - vmin) * scale + 0.5), 0, 255).astype(np.uint8)

    # Verify quantization quality
    dequant = quantized.astype(np.float32) / scale + vmin
    quant_error = np.abs(sorted_vectors - dequant).mean()
    print(f"  Quantization error (mean abs): {quant_error:.6f}")
    print(f"  vectors.bin size: {sorted_vectors.nbytes/1024/1024:.1f}MB -> {quantized.nbytes/1024/1024:.1f}MB")

    print("Computing cluster offsets...")
    offsets = np.zeros((n_clusters, 2), dtype=np.uint32)
    for c in range(n_clusters):
        mask = sorted_assignments == c
        indices = np.where(mask)[0]
        if len(indices) > 0:
            offsets[c, 0] = indices[0]
            offsets[c, 1] = len(indices)

    counts = offsets[:, 1]
    print(f"  Cluster sizes: min={counts.min()}, max={counts.max()}, "
          f"mean={counts.mean():.0f}, median={np.median(counts):.0f}")
    print(f"  Empty clusters: {(counts == 0).sum()}")

    print(f"Writing binary files to {output_dir}/...")

    centroids.tofile(os.path.join(output_dir, 'centroids.bin'))
    print(f"  centroids.bin: {centroids.nbytes} bytes")

    # Pad to 16 bytes per vector for SSE alignment (14 dims + 2 padding zeros)
    padded = np.zeros((n_vectors, 16), dtype=np.uint8)
    padded[:, :n_dims] = quantized
    padded.tofile(os.path.join(output_dir, 'vectors.bin'))
    print(f"  vectors.bin: {padded.nbytes} bytes (uint8 quantized, 16-byte aligned)")

    sorted_labels.tofile(os.path.join(output_dir, 'labels.bin'))
    print(f"  labels.bin: {sorted_labels.nbytes} bytes")

    offsets.tofile(os.path.join(output_dir, 'offsets.bin'))
    print(f"  offsets.bin: {offsets.nbytes} bytes")

    # Save quantization parameters: [min_0, scale_0, min_1, scale_1, ...]
    quant_params = np.zeros(n_dims * 2, dtype=np.float32)
    for d in range(n_dims):
        quant_params[d * 2] = vmin[d]
        quant_params[d * 2 + 1] = scale[d]
    quant_params.tofile(os.path.join(output_dir, 'quant.bin'))
    print(f"  quant.bin: {quant_params.nbytes} bytes")

    with open(os.path.join(output_dir, 'meta.bin'), 'wb') as f:
        f.write(struct.pack('III', n_vectors, n_clusters, n_dims))
    print(f"  meta.bin: 12 bytes")

    total_size = centroids.nbytes + quantized.nbytes + sorted_labels.nbytes + offsets.nbytes + quant_params.nbytes + 12
    print(f"\nTotal index size: {total_size / 1024 / 1024:.1f} MB")
    print("Done!")

if __name__ == '__main__':
    main()
