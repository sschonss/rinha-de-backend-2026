# Rinha de Backend 2026 (PHP/Swoole) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a fraud detection API using IVF vector search in PHP/Swoole with a C FFI kernel, competing in Rinha de Backend 2026.

**Architecture:** HAProxy load balancer distributes requests to 2 PHP/Swoole API instances. Each request is vectorized (14 dims), searched against 3M reference vectors using an IVF (Inverted File Index) pre-built at Docker build time. The hot path (distance computation + KNN search) runs in C via PHP FFI with SSE2 SIMD. Both instances share the index via mmap.

**Tech Stack:** PHP 8.3 + Swoole (HTTP server), C with SSE2 (vector search), Python + scikit-learn (offline indexer), HAProxy (load balancer), Docker.

---

## File Structure

```
rinha-de-backend-2026/
├── indexer/
│   ├── build_index.py          # K-means clustering, outputs binary IVF index
│   └── requirements.txt        # numpy, scikit-learn
├── src/
│   ├── vector_search.c         # C: IVF search with SSE2 SIMD + mmap
│   ├── vector_search.h         # C: header with public API
│   ├── server.php              # Swoole HTTP server entry point
│   ├── FraudDetector.php       # Vectorization (14 dims) + scoring logic
│   └── VectorSearch.php        # PHP FFI wrapper for libvector.so
├── config/
│   └── haproxy.cfg             # HAProxy round-robin configuration
├── tests/
│   ├── test_vectorization.php  # Unit tests: vectorization against spec examples
│   └── test_search.c           # C test: IVF search correctness with small dataset
├── scripts/
│   └── download_resources.sh   # Downloads reference files from competition repo
├── Dockerfile                  # Multi-stage: downloader → indexer → builder → runtime
├── docker-compose.yml          # HAProxy + 2 API instances with resource limits
├── info.json                   # Participant metadata
└── README.md
```

---

## Task 1: Project Setup & Git Init

**Files:**
- Create: `README.md`
- Create: `info.json`
- Create: `.gitignore`

- [ ] **Step 1: Initialize git repository**

```bash
cd /Users/luizschons/Documents/codes/rinha-de-backend-2026
git init
```

- [ ] **Step 2: Create .gitignore**

Create `.gitignore`:
```
resources/references.json.gz
resources/example-references.json
*.o
*.so
*.bin
__pycache__/
.DS_Store
vendor/
```

- [ ] **Step 3: Create info.json**

Create `info.json` (update participant name and social links as needed):
```json
{
    "participants": ["Luiz Schons"],
    "social": ["https://github.com/luizschons"],
    "source-code-repo": "https://github.com/luizschons/rinha-de-backend-2026",
    "stack": ["php", "swoole", "c", "haproxy"],
    "open_to_work": true
}
```

- [ ] **Step 4: Create README.md**

Create `README.md`:
```markdown
# Rinha de Backend 2026 — PHP/Swoole + C FFI

Detecção de fraude por busca vetorial usando IVF (Inverted File Index).

## Stack
- **API**: PHP 8.3 + Swoole (HTTP server async)
- **Busca vetorial**: C com SSE2 SIMD via PHP FFI
- **Indexação**: Python + scikit-learn (k-means offline)
- **Load balancer**: HAProxy

## Arquitetura
- HAProxy distribui round-robin para 2 instâncias PHP/Swoole
- IVF index pré-construído no Docker build (~1500 clusters)
- Dados compartilhados via mmap entre instâncias
- Limites: 1 CPU, 350MB RAM total

## Como rodar
```bash
docker compose up --build
curl http://localhost:9999/ready
```
```

- [ ] **Step 5: Create directory structure**

```bash
mkdir -p indexer src config tests scripts resources
```

- [ ] **Step 6: Commit**

```bash
git add .
git commit -m "chore: initial project setup

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 2: Download Resource Files

**Files:**
- Create: `scripts/download_resources.sh`
- Create: `resources/.gitkeep`

- [ ] **Step 1: Create download script**

Create `scripts/download_resources.sh`:
```bash
#!/usr/bin/env bash
set -euo pipefail

REPO_BASE="https://github.com/zanfranceschi/rinha-de-backend-2026/raw/main/resources"
DEST="resources"

mkdir -p "$DEST"

echo "Downloading references.json.gz (~16MB)..."
curl -L -o "$DEST/references.json.gz" "$REPO_BASE/references.json.gz"

echo "Downloading mcc_risk.json..."
curl -L -o "$DEST/mcc_risk.json" "$REPO_BASE/mcc_risk.json"

echo "Downloading normalization.json..."
curl -L -o "$DEST/normalization.json" "$REPO_BASE/normalization.json"

echo "Downloading example-references.json (for testing)..."
curl -L -o "$DEST/example-references.json" "$REPO_BASE/example-references.json"

echo "Done. Files in $DEST/"
ls -lh "$DEST/"
```

- [ ] **Step 2: Make executable and run**

```bash
chmod +x scripts/download_resources.sh
./scripts/download_resources.sh
```

Expected: files downloaded to `resources/` directory.

- [ ] **Step 3: Create .gitkeep for resources**

```bash
touch resources/.gitkeep
```

- [ ] **Step 4: Commit**

```bash
git add scripts/ resources/.gitkeep
git commit -m "chore: add resource download script

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 3: PHP Vectorization + Tests

**Files:**
- Create: `src/FraudDetector.php`
- Create: `tests/test_vectorization.php`

- [ ] **Step 1: Write the vectorization test**

Create `tests/test_vectorization.php`:
```php
<?php
require_once __DIR__ . '/../src/FraudDetector.php';

function assert_vector(string $name, array $input, array $expected): void {
    $actual = FraudDetector::vectorize($input);

    $pass = true;
    for ($i = 0; $i < 14; $i++) {
        if (abs($actual[$i] - $expected[$i]) > 0.001) {
            echo "FAIL [$name] dim $i: expected {$expected[$i]}, got {$actual[$i]}\n";
            $pass = false;
        }
    }
    if ($pass) {
        echo "PASS [$name]\n";
    }
}

// Test 1: Legit transaction (from spec REGRAS_DE_DETECCAO.md)
assert_vector('legit_spec_example', [
    'id' => 'tx-1329056812',
    'transaction' => ['amount' => 41.12, 'installments' => 2, 'requested_at' => '2026-03-11T18:45:53Z'],
    'customer' => ['avg_amount' => 82.24, 'tx_count_24h' => 3, 'known_merchants' => ['MERC-003', 'MERC-016']],
    'merchant' => ['id' => 'MERC-016', 'mcc' => '5411', 'avg_amount' => 60.25],
    'terminal' => ['is_online' => false, 'card_present' => true, 'km_from_home' => 29.23],
    'last_transaction' => null,
], [0.0041, 0.1667, 0.05, 0.7826, 0.3333, -1, -1, 0.0292, 0.15, 0, 1, 0, 0.15, 0.006]);

// Test 2: Fraud transaction (from spec REGRAS_DE_DETECCAO.md)
assert_vector('fraud_spec_example', [
    'id' => 'tx-3330991687',
    'transaction' => ['amount' => 9505.97, 'installments' => 10, 'requested_at' => '2026-03-14T05:15:12Z'],
    'customer' => ['avg_amount' => 81.28, 'tx_count_24h' => 20, 'known_merchants' => ['MERC-008', 'MERC-007', 'MERC-005']],
    'merchant' => ['id' => 'MERC-068', 'mcc' => '7802', 'avg_amount' => 54.86],
    'terminal' => ['is_online' => false, 'card_present' => true, 'km_from_home' => 952.27],
    'last_transaction' => null,
], [0.9506, 0.8333, 1.0, 0.2174, 0.8333, -1, -1, 0.9523, 1.0, 0, 1, 1, 0.75, 0.0055]);

// Test 3: Transaction with last_transaction (from API.md example)
assert_vector('with_last_tx', [
    'id' => 'tx-3576980410',
    'transaction' => ['amount' => 384.88, 'installments' => 3, 'requested_at' => '2026-03-11T20:23:35Z'],
    'customer' => ['avg_amount' => 769.76, 'tx_count_24h' => 3, 'known_merchants' => ['MERC-009', 'MERC-009', 'MERC-001', 'MERC-001']],
    'merchant' => ['id' => 'MERC-001', 'mcc' => '5912', 'avg_amount' => 298.95],
    'terminal' => ['is_online' => false, 'card_present' => true, 'km_from_home' => 13.7090520965],
    'last_transaction' => ['timestamp' => '2026-03-11T14:58:35Z', 'km_from_current' => 18.8626479774],
], [
    0.0385,   // 384.88 / 10000
    0.25,     // 3 / 12
    0.05,     // (384.88 / 769.76) / 10 = 0.5 / 10
    0.8696,   // 20 / 23
    0.3333,   // Wed=2, 2/6
    0.2257,   // minutes: (20:23:35 - 14:58:35) = 325 min, 325/1440
    0.0189,   // 18.8626 / 1000
    0.0137,   // 13.709 / 1000
    0.15,     // 3 / 20
    0,        // is_online=false
    1,        // card_present=true
    0,        // MERC-001 in known_merchants → known → 0
    0.20,     // mcc_risk[5912]
    0.0299,   // 298.95 / 10000
]);

// Test 4: Edge case — avg_amount = 0 (division by zero protection)
assert_vector('division_by_zero', [
    'id' => 'tx-edge',
    'transaction' => ['amount' => 100.0, 'installments' => 1, 'requested_at' => '2026-01-05T12:00:00Z'],
    'customer' => ['avg_amount' => 0, 'tx_count_24h' => 0, 'known_merchants' => []],
    'merchant' => ['id' => 'MERC-999', 'mcc' => '9999', 'avg_amount' => 0],
    'terminal' => ['is_online' => true, 'card_present' => false, 'km_from_home' => 0],
    'last_transaction' => null,
], [
    0.01,     // 100 / 10000
    0.0833,   // 1 / 12
    1.0,      // clamp(100/0 → INF → 1.0)
    0.5217,   // 12 / 23
    0.0,      // Mon=0, 0/6
    -1, -1,
    0.0,      // 0 / 1000
    0.0,      // 0 / 20
    1,        // is_online=true
    0,        // card_present=false
    1,        // MERC-999 not in [] → unknown → 1
    0.5,      // mcc 9999 not in map → default 0.5
    0.0,      // 0 / 10000
]);

echo "\nAll vectorization tests done.\n";
```

- [ ] **Step 2: Run test to verify it fails**

```bash
php tests/test_vectorization.php
```

Expected: FAIL — `FraudDetector` class not found.

- [ ] **Step 3: Implement FraudDetector.php**

Create `src/FraudDetector.php`:
```php
<?php
date_default_timezone_set('UTC');

class FraudDetector
{
    const MAX_AMOUNT = 10000;
    const MAX_INSTALLMENTS = 12;
    const AMOUNT_VS_AVG_RATIO = 10;
    const MAX_MINUTES = 1440;
    const MAX_KM = 1000;
    const MAX_TX_COUNT_24H = 20;
    const MAX_MERCHANT_AVG = 10000;

    const MCC_RISK = [
        '5411' => 0.15, '5812' => 0.30, '5912' => 0.20,
        '5944' => 0.45, '7801' => 0.80, '7802' => 0.75,
        '7995' => 0.85, '4511' => 0.35, '5311' => 0.25,
        '5999' => 0.50,
    ];

    public static function score(array $data): array
    {
        $vector = self::vectorize($data);
        $labels = VectorSearch::query($vector);
        $fraudCount = 0;
        for ($i = 0; $i < 5; $i++) {
            $fraudCount += $labels[$i];
        }
        $fraudScore = $fraudCount / 5.0;

        return [
            'approved' => $fraudScore < 0.6,
            'fraud_score' => $fraudScore,
        ];
    }

    public static function vectorize(array $d): array
    {
        $tx = $d['transaction'];
        $cust = $d['customer'];
        $merch = $d['merchant'];
        $term = $d['terminal'];
        $last = $d['last_transaction'];

        $ts = strtotime($tx['requested_at']);
        $hour = (int) gmdate('G', $ts);
        $dow = ((int) gmdate('N', $ts)) - 1; // Mon=0, Sun=6

        $avgAmount = $cust['avg_amount'];
        $amountVsAvg = ($avgAmount > 0)
            ? ($tx['amount'] / $avgAmount) / self::AMOUNT_VS_AVG_RATIO
            : 1.0; // division by zero → clamp to 1.0

        if ($last !== null) {
            $lastTs = strtotime($last['timestamp']);
            $minutes = ($ts - $lastTs) / 60.0;
            $minutesSinceLast = self::clamp($minutes / self::MAX_MINUTES);
            $kmFromLast = self::clamp($last['km_from_current'] / self::MAX_KM);
        } else {
            $minutesSinceLast = -1.0;
            $kmFromLast = -1.0;
        }

        return [
            self::clamp($tx['amount'] / self::MAX_AMOUNT),                    // 0: amount
            self::clamp($tx['installments'] / self::MAX_INSTALLMENTS),        // 1: installments
            self::clamp($amountVsAvg),                                        // 2: amount_vs_avg
            $hour / 23.0,                                                     // 3: hour_of_day
            $dow / 6.0,                                                       // 4: day_of_week
            $minutesSinceLast,                                                // 5: minutes_since_last_tx
            $kmFromLast,                                                      // 6: km_from_last_tx
            self::clamp($term['km_from_home'] / self::MAX_KM),               // 7: km_from_home
            self::clamp($cust['tx_count_24h'] / self::MAX_TX_COUNT_24H),     // 8: tx_count_24h
            $term['is_online'] ? 1.0 : 0.0,                                  // 9: is_online
            $term['card_present'] ? 1.0 : 0.0,                               // 10: card_present
            in_array($merch['id'], $cust['known_merchants']) ? 0.0 : 1.0,    // 11: unknown_merchant
            self::MCC_RISK[$merch['mcc']] ?? 0.5,                            // 12: mcc_risk
            self::clamp($merch['avg_amount'] / self::MAX_MERCHANT_AVG),      // 13: merchant_avg_amount
        ];
    }

    private static function clamp(float $v): float
    {
        return max(0.0, min(1.0, $v));
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
php tests/test_vectorization.php
```

Expected: All 4 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add src/FraudDetector.php tests/test_vectorization.php
git commit -m "feat: implement vectorization with 14-dim normalization

Tested against spec examples from REGRAS_DE_DETECCAO.md.
Handles edge cases: null last_transaction, unknown MCC, division by zero.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 4: Python Indexer (K-Means IVF Index Builder)

**Files:**
- Create: `indexer/requirements.txt`
- Create: `indexer/build_index.py`

- [ ] **Step 1: Create requirements.txt**

Create `indexer/requirements.txt`:
```
numpy>=1.24
scikit-learn>=1.3
```

- [ ] **Step 2: Write build_index.py**

Create `indexer/build_index.py`:
```python
#!/usr/bin/env python3
"""
Builds an IVF (Inverted File Index) from references.json.gz.

Reads 3M labeled vectors, clusters them via MiniBatchKMeans,
and outputs binary files for mmap-based loading at runtime.

Output files:
  - centroids.bin   : float32[n_clusters × 14] — cluster centroids
  - vectors.bin     : float32[n_vectors × 14]  — vectors sorted by cluster
  - labels.bin      : uint8[n_vectors]          — labels sorted by cluster (0=legit, 1=fraud)
  - offsets.bin      : uint32[n_clusters × 2]   — (start_index, count) per cluster
  - meta.bin        : uint32[3]                 — (n_vectors, n_clusters, n_dims)
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

    print("Computing cluster offsets...")
    offsets = np.zeros((n_clusters, 2), dtype=np.uint32)
    for c in range(n_clusters):
        mask = sorted_assignments == c
        indices = np.where(mask)[0]
        if len(indices) > 0:
            offsets[c, 0] = indices[0]      # start
            offsets[c, 1] = len(indices)    # count
        # else: (0, 0) — empty cluster

    # Print cluster stats
    counts = offsets[:, 1]
    print(f"  Cluster sizes: min={counts.min()}, max={counts.max()}, "
          f"mean={counts.mean():.0f}, median={np.median(counts):.0f}")
    print(f"  Empty clusters: {(counts == 0).sum()}")

    print(f"Writing binary files to {output_dir}/...")

    centroids.tofile(os.path.join(output_dir, 'centroids.bin'))
    print(f"  centroids.bin: {centroids.nbytes} bytes")

    sorted_vectors.tofile(os.path.join(output_dir, 'vectors.bin'))
    print(f"  vectors.bin: {sorted_vectors.nbytes} bytes")

    sorted_labels.tofile(os.path.join(output_dir, 'labels.bin'))
    print(f"  labels.bin: {sorted_labels.nbytes} bytes")

    offsets.tofile(os.path.join(output_dir, 'offsets.bin'))
    print(f"  offsets.bin: {offsets.nbytes} bytes")

    with open(os.path.join(output_dir, 'meta.bin'), 'wb') as f:
        f.write(struct.pack('III', n_vectors, n_clusters, n_dims))
    print(f"  meta.bin: 12 bytes")

    total_size = centroids.nbytes + sorted_vectors.nbytes + sorted_labels.nbytes + offsets.nbytes + 12
    print(f"\nTotal index size: {total_size / 1024 / 1024:.1f} MB")
    print("Done!")

if __name__ == '__main__':
    main()
```

- [ ] **Step 3: Test the indexer with the example references (small dataset)**

```bash
cd /Users/luizschons/Documents/codes/rinha-de-backend-2026
pip3 install -r indexer/requirements.txt
python3 indexer/build_index.py resources/example-references.json data-test 10
```

Expected: outputs binary files to `data-test/` with a small number of vectors and 10 clusters. Verify file sizes make sense.

- [ ] **Step 4: Clean up test output and commit**

```bash
rm -rf data-test
git add indexer/
git commit -m "feat: add Python IVF index builder (k-means clustering)

MiniBatchKMeans clusters 3M vectors, outputs mmap-friendly binary files.
Cluster count configurable (default 1500).

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 5: C Vector Search Library

**Files:**
- Create: `src/vector_search.h`
- Create: `src/vector_search.c`

- [ ] **Step 1: Write the header file**

Create `src/vector_search.h`:
```c
#ifndef VECTOR_SEARCH_H
#define VECTOR_SEARCH_H

#include <stdint.h>

#define IVF_DIMS 14
#define IVF_K 5

// Initialize the IVF index from binary files in index_dir.
// nprobe = number of clusters to search per query.
// Returns 0 on success, -1 on failure.
int ivf_init(const char *index_dir, int nprobe);

// Search for k nearest neighbors of query vector.
// out_labels: array of k labels (0=legit, 1=fraud)
// out_distances: array of k squared distances (for debugging, can be NULL)
// Returns 0 on success, -1 on failure.
int ivf_search(const float *query, int *out_labels, float *out_distances, int k);

// Free resources.
void ivf_destroy(void);

#endif
```

- [ ] **Step 2: Write the C implementation**

Create `src/vector_search.c`:
```c
#include "vector_search.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <xmmintrin.h>
#include <emmintrin.h>

// --- Index data (mmap'd) ---

typedef struct {
    float    *centroids;    // [n_clusters * DIMS]
    float    *vectors;      // [n_vectors * DIMS]
    uint8_t  *labels;       // [n_vectors]
    uint32_t *offsets;      // [n_clusters * 2] (start, count)
    uint32_t  n_vectors;
    uint32_t  n_clusters;
    uint32_t  n_dims;
    int       nprobe;

    // mmap bookkeeping
    void   *mmap_ptrs[5];
    size_t  mmap_sizes[5];
    int     n_mmaps;
} IVFIndex;

static IVFIndex g_idx;

// --- mmap helper ---

static void *mmap_file(const char *path, size_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return NULL;
    }
    *out_size = (size_t)st.st_size;
    void *ptr = mmap(NULL, *out_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    // Advise sequential access for initial loading
    madvise(ptr, *out_size, MADV_SEQUENTIAL);
    return ptr;
}

// --- Distance computation (SSE2) ---

static inline float dist_euclidean_sse2(const float *a, const float *b) {
    __m128 sum = _mm_setzero_ps();

    // 3 blocks of 4 dims = 12 dims
    __m128 va, vb, diff;

    va = _mm_loadu_ps(a);
    vb = _mm_loadu_ps(b);
    diff = _mm_sub_ps(va, vb);
    sum = _mm_add_ps(sum, _mm_mul_ps(diff, diff));

    va = _mm_loadu_ps(a + 4);
    vb = _mm_loadu_ps(b + 4);
    diff = _mm_sub_ps(va, vb);
    sum = _mm_add_ps(sum, _mm_mul_ps(diff, diff));

    va = _mm_loadu_ps(a + 8);
    vb = _mm_loadu_ps(b + 8);
    diff = _mm_sub_ps(va, vb);
    sum = _mm_add_ps(sum, _mm_mul_ps(diff, diff));

    // Horizontal sum of 4 floats in SSE register
    __m128 shuf = _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(2, 3, 0, 1));
    __m128 sums = _mm_add_ps(sum, shuf);
    shuf = _mm_movehl_ps(shuf, sums);
    sums = _mm_add_ss(sums, shuf);
    float s;
    _mm_store_ss(&s, sums);

    // Last 2 dims (12 and 13) — scalar
    float d12 = a[12] - b[12];
    float d13 = a[13] - b[13];
    s += d12 * d12 + d13 * d13;

    return s;
}

// --- Max-heap of size K for top-K nearest neighbors ---

typedef struct {
    float    dist;
    uint32_t idx;
} HeapItem;

static inline void heap_sift_down(HeapItem *heap, int size, int i) {
    while (1) {
        int largest = i;
        int left = 2 * i + 1;
        int right = 2 * i + 2;
        if (left < size && heap[left].dist > heap[largest].dist) largest = left;
        if (right < size && heap[right].dist > heap[largest].dist) largest = right;
        if (largest == i) break;
        HeapItem tmp = heap[i];
        heap[i] = heap[largest];
        heap[largest] = tmp;
        i = largest;
    }
}

static inline void heap_sift_up(HeapItem *heap, int i) {
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (heap[parent].dist < heap[i].dist) {
            HeapItem tmp = heap[parent];
            heap[parent] = heap[i];
            heap[i] = tmp;
            i = parent;
        } else break;
    }
}

static inline void heap_push(HeapItem *heap, int *size, int max_size,
                              float dist, uint32_t idx) {
    if (*size < max_size) {
        heap[*size].dist = dist;
        heap[*size].idx = idx;
        (*size)++;
        heap_sift_up(heap, *size - 1);
    } else if (dist < heap[0].dist) {
        heap[0].dist = dist;
        heap[0].idx = idx;
        heap_sift_down(heap, max_size, 0);
    }
}

// --- Public API ---

int ivf_init(const char *index_dir, int nprobe) {
    char path[512];
    memset(&g_idx, 0, sizeof(g_idx));
    g_idx.nprobe = nprobe > 0 ? nprobe : 10;

    // Load meta
    snprintf(path, sizeof(path), "%s/meta.bin", index_dir);
    size_t meta_size;
    uint32_t *meta = (uint32_t *)mmap_file(path, &meta_size);
    if (!meta) return -1;
    g_idx.n_vectors  = meta[0];
    g_idx.n_clusters = meta[1];
    g_idx.n_dims     = meta[2];
    g_idx.mmap_ptrs[g_idx.n_mmaps] = meta;
    g_idx.mmap_sizes[g_idx.n_mmaps] = meta_size;
    g_idx.n_mmaps++;

    fprintf(stderr, "[ivf] n_vectors=%u, n_clusters=%u, n_dims=%u, nprobe=%d\n",
            g_idx.n_vectors, g_idx.n_clusters, g_idx.n_dims, g_idx.nprobe);

    // Load centroids
    snprintf(path, sizeof(path), "%s/centroids.bin", index_dir);
    size_t sz;
    g_idx.centroids = (float *)mmap_file(path, &sz);
    if (!g_idx.centroids) return -1;
    g_idx.mmap_ptrs[g_idx.n_mmaps] = g_idx.centroids;
    g_idx.mmap_sizes[g_idx.n_mmaps] = sz;
    g_idx.n_mmaps++;

    // Load vectors
    snprintf(path, sizeof(path), "%s/vectors.bin", index_dir);
    g_idx.vectors = (float *)mmap_file(path, &sz);
    if (!g_idx.vectors) return -1;
    g_idx.mmap_ptrs[g_idx.n_mmaps] = g_idx.vectors;
    g_idx.mmap_sizes[g_idx.n_mmaps] = sz;
    g_idx.n_mmaps++;

    // Load labels
    snprintf(path, sizeof(path), "%s/labels.bin", index_dir);
    g_idx.labels = (uint8_t *)mmap_file(path, &sz);
    if (!g_idx.labels) return -1;
    g_idx.mmap_ptrs[g_idx.n_mmaps] = g_idx.labels;
    g_idx.mmap_sizes[g_idx.n_mmaps] = sz;
    g_idx.n_mmaps++;

    // Load offsets
    snprintf(path, sizeof(path), "%s/offsets.bin", index_dir);
    g_idx.offsets = (uint32_t *)mmap_file(path, &sz);
    if (!g_idx.offsets) return -1;
    g_idx.mmap_ptrs[g_idx.n_mmaps] = g_idx.offsets;
    g_idx.mmap_sizes[g_idx.n_mmaps] = sz;
    g_idx.n_mmaps++;

    // Touch all pages to force them into RAM
    volatile uint8_t dummy = 0;
    for (size_t i = 0; i < g_idx.n_vectors; i += 4096 / (IVF_DIMS * sizeof(float))) {
        dummy += ((volatile uint8_t *)g_idx.vectors)[i * IVF_DIMS * sizeof(float)];
    }
    for (size_t i = 0; i < g_idx.n_vectors; i += 4096) {
        dummy += ((volatile uint8_t *)g_idx.labels)[i];
    }
    (void)dummy;

    fprintf(stderr, "[ivf] index loaded successfully\n");
    return 0;
}

int ivf_search(const float *query, int *out_labels, float *out_distances, int k) {
    if (!g_idx.centroids) return -1;
    int nprobe = g_idx.nprobe;
    uint32_t n_clusters = g_idx.n_clusters;

    // Step 1: Find top-nprobe closest centroids
    // Using simple insertion sort for small nprobe
    float  cent_dists[64];
    int    cent_ids[64];
    int    n_found = 0;

    if (nprobe > 64) nprobe = 64;

    for (uint32_t c = 0; c < n_clusters; c++) {
        float d = dist_euclidean_sse2(query, g_idx.centroids + c * IVF_DIMS);

        if (n_found < nprobe) {
            // Insert sorted
            int pos = n_found;
            while (pos > 0 && d < cent_dists[pos - 1]) {
                cent_dists[pos] = cent_dists[pos - 1];
                cent_ids[pos] = cent_ids[pos - 1];
                pos--;
            }
            cent_dists[pos] = d;
            cent_ids[pos] = c;
            n_found++;
        } else if (d < cent_dists[n_found - 1]) {
            int pos = n_found - 1;
            while (pos > 0 && d < cent_dists[pos - 1]) {
                cent_dists[pos] = cent_dists[pos - 1];
                cent_ids[pos] = cent_ids[pos - 1];
                pos--;
            }
            cent_dists[pos] = d;
            cent_ids[pos] = c;
        }
    }

    // Step 2: Search within top-nprobe clusters for k nearest neighbors
    HeapItem heap[IVF_K];
    int heap_size = 0;

    for (int p = 0; p < nprobe; p++) {
        int c = cent_ids[p];
        uint32_t start = g_idx.offsets[c * 2];
        uint32_t count = g_idx.offsets[c * 2 + 1];

        for (uint32_t i = 0; i < count; i++) {
            uint32_t vidx = start + i;
            float d = dist_euclidean_sse2(query, g_idx.vectors + vidx * IVF_DIMS);
            heap_push(heap, &heap_size, k, d, vidx);
        }
    }

    // Step 3: Extract results from heap
    for (int i = 0; i < k; i++) {
        if (i < heap_size) {
            out_labels[i] = g_idx.labels[heap[i].idx];
            if (out_distances) out_distances[i] = heap[i].dist;
        } else {
            out_labels[i] = 0;
            if (out_distances) out_distances[i] = 0.0f;
        }
    }

    return 0;
}

void ivf_destroy(void) {
    for (int i = 0; i < g_idx.n_mmaps; i++) {
        if (g_idx.mmap_ptrs[i]) {
            munmap(g_idx.mmap_ptrs[i], g_idx.mmap_sizes[i]);
        }
    }
    memset(&g_idx, 0, sizeof(g_idx));
}
```

- [ ] **Step 3: Compile the library**

```bash
gcc -O3 -msse2 -shared -fPIC -o src/libvector.so src/vector_search.c
```

Expected: `src/libvector.so` created without errors.

- [ ] **Step 4: Commit**

```bash
git add src/vector_search.h src/vector_search.c
git commit -m "feat: implement C IVF vector search with SSE2 SIMD

mmap-based index loading, max-heap for top-K, centroid probing.
Euclidean distance without sqrt using SSE2 intrinsics.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 6: C Library Tests

**Files:**
- Create: `tests/test_search.c`

- [ ] **Step 1: Write C test program**

Create `tests/test_search.c`:
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <sys/stat.h>

// Test the distance function and IVF search with a small synthetic dataset.
// We create binary index files in a temp directory.

#define DIMS 14
#define N_VECTORS 100
#define N_CLUSTERS 5

static void write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    fwrite(data, 1, size, f);
    fclose(f);
}

int main(void) {
    char dir[] = "/tmp/ivf_test_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }

    // Create synthetic data: 5 clusters of 20 vectors each
    float vectors[N_VECTORS * DIMS];
    uint8_t labels[N_VECTORS];
    float centroids[N_CLUSTERS * DIMS];
    uint32_t offsets[N_CLUSTERS * 2];

    srand(42);
    for (int c = 0; c < N_CLUSTERS; c++) {
        // Each centroid is at (c*0.2, c*0.2, ...) for all 14 dims
        for (int d = 0; d < DIMS; d++) {
            centroids[c * DIMS + d] = c * 0.2f;
        }
        offsets[c * 2] = c * 20;      // start
        offsets[c * 2 + 1] = 20;      // count

        for (int i = 0; i < 20; i++) {
            int idx = c * 20 + i;
            for (int d = 0; d < DIMS; d++) {
                // Vector = centroid + small noise
                vectors[idx * DIMS + d] = c * 0.2f + (rand() % 100 - 50) * 0.001f;
            }
            // First cluster = fraud, rest = legit
            labels[idx] = (c == 0) ? 1 : 0;
        }
    }

    // Write binary files
    char path[512];
    snprintf(path, sizeof(path), "%s/centroids.bin", dir);
    write_file(path, centroids, sizeof(centroids));

    snprintf(path, sizeof(path), "%s/vectors.bin", dir);
    write_file(path, vectors, sizeof(vectors));

    snprintf(path, sizeof(path), "%s/labels.bin", dir);
    write_file(path, labels, sizeof(labels));

    snprintf(path, sizeof(path), "%s/offsets.bin", dir);
    write_file(path, offsets, sizeof(offsets));

    uint32_t meta[3] = {N_VECTORS, N_CLUSTERS, DIMS};
    snprintf(path, sizeof(path), "%s/meta.bin", dir);
    write_file(path, meta, sizeof(meta));

    // Include the actual implementation
    // We test via the shared library interface
    // Declare functions from vector_search.h
    extern int ivf_init(const char *index_dir, int nprobe);
    extern int ivf_search(const float *query, int *out_labels, float *out_distances, int k);
    extern void ivf_destroy(void);

    // Init
    int ret = ivf_init(dir, 3);
    if (ret != 0) {
        printf("FAIL: ivf_init returned %d\n", ret);
        return 1;
    }
    printf("PASS: ivf_init\n");

    // Test 1: Query near cluster 0 (fraud cluster) should return mostly fraud
    float query_fraud[DIMS];
    for (int d = 0; d < DIMS; d++) query_fraud[d] = 0.01f;

    int result_labels[5];
    float result_dists[5];
    ret = ivf_search(query_fraud, result_labels, result_dists, 5);
    if (ret != 0) { printf("FAIL: ivf_search returned %d\n", ret); return 1; }

    int fraud_count = 0;
    for (int i = 0; i < 5; i++) fraud_count += result_labels[i];
    printf("Query near cluster 0 (fraud): %d/5 fraud labels\n", fraud_count);
    if (fraud_count >= 4) {
        printf("PASS: fraud detection\n");
    } else {
        printf("FAIL: expected mostly fraud, got %d/5\n", fraud_count);
    }

    // Test 2: Query near cluster 3 (legit) should return mostly legit
    float query_legit[DIMS];
    for (int d = 0; d < DIMS; d++) query_legit[d] = 0.6f;

    ret = ivf_search(query_legit, result_labels, result_dists, 5);
    if (ret != 0) { printf("FAIL: ivf_search returned %d\n", ret); return 1; }

    int legit_count = 0;
    for (int i = 0; i < 5; i++) legit_count += (result_labels[i] == 0);
    printf("Query near cluster 3 (legit): %d/5 legit labels\n", legit_count);
    if (legit_count >= 4) {
        printf("PASS: legit detection\n");
    } else {
        printf("FAIL: expected mostly legit, got %d/5\n", legit_count);
    }

    // Test 3: Distances should be non-negative and sorted
    int sorted = 1;
    for (int i = 0; i < 5; i++) {
        if (result_dists[i] < 0) { sorted = 0; break; }
    }
    printf("%s: distances non-negative\n", sorted ? "PASS" : "FAIL");

    ivf_destroy();
    printf("PASS: ivf_destroy\n");

    // Cleanup temp files
    snprintf(path, sizeof(path), "rm -rf %s", dir);
    system(path);

    printf("\nAll C tests done.\n");
    return 0;
}
```

- [ ] **Step 2: Compile and run the test**

```bash
gcc -O3 -msse2 -o tests/test_search tests/test_search.c src/vector_search.c -lm
./tests/test_search
```

Expected: All tests PASS.

- [ ] **Step 3: Commit**

```bash
rm -f tests/test_search  # don't commit binary
git add tests/test_search.c
git commit -m "test: add C IVF search tests with synthetic dataset

Tests fraud/legit detection, distance correctness, and init/destroy.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 7: PHP FFI Wrapper

**Files:**
- Create: `src/VectorSearch.php`

- [ ] **Step 1: Write VectorSearch.php**

Create `src/VectorSearch.php`:
```php
<?php

class VectorSearch
{
    private static ?\FFI $ffi = null;
    private static ?\FFI\CData $queryBuf = null;
    private static ?\FFI\CData $labelsBuf = null;
    private static ?\FFI\CData $distsBuf = null;
    private static bool $initialized = false;

    public static function init(string $indexDir, string $libPath = null, int $nprobe = 10): void
    {
        if (self::$initialized) return;

        $libPath = $libPath ?? __DIR__ . '/libvector.so';

        self::$ffi = \FFI::cdef("
            int ivf_init(const char *index_dir, int nprobe);
            int ivf_search(const float *query, int *out_labels, float *out_distances, int k);
            void ivf_destroy(void);
        ", $libPath);

        $result = self::$ffi->ivf_init($indexDir, $nprobe);
        if ($result !== 0) {
            throw new \RuntimeException("Failed to initialize IVF index from $indexDir");
        }

        // Pre-allocate buffers to avoid per-request allocation
        self::$queryBuf = self::$ffi->new("float[14]");
        self::$labelsBuf = self::$ffi->new("int[5]");
        self::$distsBuf = self::$ffi->new("float[5]");
        self::$initialized = true;
    }

    /**
     * @param float[] $vector Array of 14 floats
     * @return int[] Array of 5 labels (0=legit, 1=fraud)
     */
    public static function query(array $vector): array
    {
        if (!self::$initialized) {
            throw new \RuntimeException("VectorSearch not initialized. Call init() first.");
        }

        for ($i = 0; $i < 14; $i++) {
            self::$queryBuf[$i] = $vector[$i];
        }

        self::$ffi->ivf_search(self::$queryBuf, self::$labelsBuf, self::$distsBuf, 5);

        $labels = [];
        for ($i = 0; $i < 5; $i++) {
            $labels[] = self::$labelsBuf[$i];
        }
        return $labels;
    }

    public static function destroy(): void
    {
        if (self::$ffi && self::$initialized) {
            self::$ffi->ivf_destroy();
            self::$initialized = false;
        }
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add src/VectorSearch.php
git commit -m "feat: add PHP FFI wrapper for C vector search library

Pre-allocates FFI buffers to avoid per-request allocation overhead.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 8: Swoole HTTP Server

**Files:**
- Create: `src/server.php`

- [ ] **Step 1: Write server.php**

Create `src/server.php`:
```php
<?php
date_default_timezone_set('UTC');

require_once __DIR__ . '/FraudDetector.php';
require_once __DIR__ . '/VectorSearch.php';

$indexDir = getenv('INDEX_DIR') ?: '/data/index';
$libPath  = getenv('LIB_PATH') ?: __DIR__ . '/libvector.so';
$nprobe   = (int)(getenv('NPROBE') ?: 10);
$port     = (int)(getenv('PORT') ?: 9999);
$workers  = (int)(getenv('WORKERS') ?: 2);

$server = new Swoole\Http\Server('0.0.0.0', $port);

$server->set([
    'worker_num'        => $workers,
    'dispatch_mode'     => 1,       // round-robin to workers
    'open_tcp_nodelay'  => true,
    'log_level'         => SWOOLE_LOG_WARNING,
    'log_file'          => '/dev/null',
]);

$ready = false;

$server->on('workerStart', function ($server, $workerId) use ($indexDir, $libPath, $nprobe, &$ready) {
    try {
        VectorSearch::init($indexDir, $libPath, $nprobe);
        $ready = true;
        echo "[worker $workerId] IVF index loaded. Ready.\n";
    } catch (\Throwable $e) {
        echo "[worker $workerId] ERROR loading index: {$e->getMessage()}\n";
    }
});

$server->on('request', function (Swoole\Http\Request $req, Swoole\Http\Response $res) use (&$ready) {
    $uri = $req->server['request_uri'];

    // GET /ready
    if ($uri === '/ready') {
        if ($ready) {
            $res->status(200);
            $res->end('OK');
        } else {
            $res->status(503);
            $res->end('NOT READY');
        }
        return;
    }

    // POST /fraud-score
    if ($uri === '/fraud-score' && $req->server['request_method'] === 'POST') {
        try {
            $data = json_decode($req->rawContent(), true);
            if (!$data) {
                // Malformed JSON — fallback
                $res->header('Content-Type', 'application/json');
                $res->end('{"approved":true,"fraud_score":0.0}');
                return;
            }
            $result = FraudDetector::score($data);
            $res->header('Content-Type', 'application/json');
            $res->end(json_encode($result));
        } catch (\Throwable $e) {
            // Any error — fallback to avoid HTTP 500 (weight=5 in scoring)
            $res->header('Content-Type', 'application/json');
            $res->end('{"approved":true,"fraud_score":0.0}');
        }
        return;
    }

    $res->status(404);
    $res->end();
});

echo "Starting Swoole server on port $port with $workers workers...\n";
$server->start();
```

- [ ] **Step 2: Commit**

```bash
git add src/server.php
git commit -m "feat: add Swoole HTTP server with /ready and /fraud-score

Defensive error handling — never returns HTTP 500 (falls back to approved:true).
Configurable via env vars: INDEX_DIR, LIB_PATH, NPROBE, PORT, WORKERS.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 9: HAProxy Configuration

**Files:**
- Create: `config/haproxy.cfg`

- [ ] **Step 1: Write haproxy.cfg**

Create `config/haproxy.cfg`:
```
global
    maxconn 1024

defaults
    mode http
    timeout connect 5s
    timeout client  5s
    timeout server  5s
    option http-keep-alive

frontend http_front
    bind *:9999
    default_backend api_servers

backend api_servers
    balance roundrobin
    option httpchk GET /ready
    http-check expect status 200
    server api1 api-1:9999 check inter 2s
    server api2 api-2:9999 check inter 2s
```

- [ ] **Step 2: Commit**

```bash
git add config/haproxy.cfg
git commit -m "feat: add HAProxy config for round-robin load balancing

Health checks via GET /ready on each API instance.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 10: Dockerfile (Multi-Stage Build)

**Files:**
- Create: `Dockerfile`

- [ ] **Step 1: Write the Dockerfile**

Create `Dockerfile`:
```dockerfile
# ============================================================
# Stage 1: Download reference files
# ============================================================
FROM alpine:3.19 AS downloader

RUN apk add --no-cache curl
WORKDIR /resources

RUN curl -L -o references.json.gz \
    https://github.com/zanfranceschi/rinha-de-backend-2026/raw/main/resources/references.json.gz

# ============================================================
# Stage 2: Build IVF index (k-means clustering)
# ============================================================
FROM python:3.11-slim AS indexer

WORKDIR /build
COPY indexer/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY indexer/build_index.py .
COPY --from=downloader /resources/references.json.gz /resources/

ARG N_CLUSTERS=1500
RUN python build_index.py /resources/references.json.gz /index ${N_CLUSTERS}

# ============================================================
# Stage 3: Compile C vector search library
# ============================================================
FROM gcc:13-bookworm AS builder

WORKDIR /build
COPY src/vector_search.h src/vector_search.c ./
RUN gcc -O3 -msse2 -shared -fPIC -o libvector.so vector_search.c

# ============================================================
# Stage 4: Runtime
# ============================================================
FROM phpswoole/swoole:php8.3

# Install FFI (usually already available, but ensure it)
RUN docker-php-ext-enable ffi 2>/dev/null || true

# Set FFI to allow preloading
RUN echo "ffi.enable=true" >> /usr/local/etc/php/conf.d/ffi.ini

WORKDIR /app

# Copy IVF index binary files
COPY --from=indexer /index /data/index

# Copy C shared library
COPY --from=builder /build/libvector.so /app/src/libvector.so

# Copy PHP source
COPY src/ /app/src/

# Environment defaults
ENV INDEX_DIR=/data/index
ENV LIB_PATH=/app/src/libvector.so
ENV NPROBE=10
ENV PORT=9999
ENV WORKERS=2

EXPOSE 9999

CMD ["php", "/app/src/server.php"]
```

- [ ] **Step 2: Commit**

```bash
git add Dockerfile
git commit -m "feat: add multi-stage Dockerfile

Stage 1: download references.json.gz
Stage 2: Python k-means indexer → binary IVF index
Stage 3: compile C vector search library
Stage 4: PHP/Swoole runtime with FFI

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 11: Docker Compose

**Files:**
- Create: `docker-compose.yml`

- [ ] **Step 1: Write docker-compose.yml**

Create `docker-compose.yml`:
```yaml
services:
  haproxy:
    image: haproxy:2.9-alpine
    ports:
      - "9999:9999"
    volumes:
      - ./config/haproxy.cfg:/usr/local/etc/haproxy/haproxy.cfg:ro
    depends_on:
      api-1:
        condition: service_started
      api-2:
        condition: service_started
    deploy:
      resources:
        limits:
          cpus: "0.05"
          memory: "10MB"
    networks:
      - rinha

  api-1:
    build:
      context: .
      dockerfile: Dockerfile
      platforms:
        - linux/amd64
    environment:
      - INDEX_DIR=/data/index
      - LIB_PATH=/app/src/libvector.so
      - NPROBE=10
      - WORKERS=2
    deploy:
      resources:
        limits:
          cpus: "0.475"
          memory: "170MB"
    networks:
      - rinha

  api-2:
    build:
      context: .
      dockerfile: Dockerfile
      platforms:
        - linux/amd64
    environment:
      - INDEX_DIR=/data/index
      - LIB_PATH=/app/src/libvector.so
      - NPROBE=10
      - WORKERS=2
    deploy:
      resources:
        limits:
          cpus: "0.475"
          memory: "170MB"
    networks:
      - rinha

networks:
  rinha:
    driver: bridge
```

- [ ] **Step 2: Commit**

```bash
git add docker-compose.yml
git commit -m "feat: add docker-compose with HAProxy + 2 API instances

Resource limits: 0.05 CPU + 10MB (HAProxy), 0.475 CPU + 170MB (each API).
Total: 1 CPU, 350MB RAM.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 12: Integration Testing

**Files:**
- Create: `scripts/test_integration.sh`

- [ ] **Step 1: Write integration test script**

Create `scripts/test_integration.sh`:
```bash
#!/usr/bin/env bash
set -euo pipefail

BASE_URL="http://localhost:9999"
PASS=0
FAIL=0

check() {
    local name="$1" expected="$2" actual="$3"
    if [[ "$actual" == *"$expected"* ]]; then
        echo "PASS: $name"
        ((PASS++))
    else
        echo "FAIL: $name — expected '$expected', got '$actual'"
        ((FAIL++))
    fi
}

echo "=== Integration Tests ==="
echo ""

# Test 1: /ready endpoint
echo "--- Testing /ready ---"
READY=$(curl -s -o /dev/null -w "%{http_code}" "$BASE_URL/ready")
check "/ready returns 200" "200" "$READY"

# Test 2: /fraud-score with legit transaction (from spec)
echo ""
echo "--- Testing /fraud-score (legit) ---"
RESPONSE=$(curl -s -X POST "$BASE_URL/fraud-score" \
    -H "Content-Type: application/json" \
    -d '{
        "id": "tx-1329056812",
        "transaction": {"amount": 41.12, "installments": 2, "requested_at": "2026-03-11T18:45:53Z"},
        "customer": {"avg_amount": 82.24, "tx_count_24h": 3, "known_merchants": ["MERC-003", "MERC-016"]},
        "merchant": {"id": "MERC-016", "mcc": "5411", "avg_amount": 60.25},
        "terminal": {"is_online": false, "card_present": true, "km_from_home": 29.23},
        "last_transaction": null
    }')
echo "Response: $RESPONSE"
check "has approved field" "approved" "$RESPONSE"
check "has fraud_score field" "fraud_score" "$RESPONSE"

# Test 3: /fraud-score with fraud transaction (from spec)
echo ""
echo "--- Testing /fraud-score (fraud) ---"
RESPONSE=$(curl -s -X POST "$BASE_URL/fraud-score" \
    -H "Content-Type: application/json" \
    -d '{
        "id": "tx-3330991687",
        "transaction": {"amount": 9505.97, "installments": 10, "requested_at": "2026-03-14T05:15:12Z"},
        "customer": {"avg_amount": 81.28, "tx_count_24h": 20, "known_merchants": ["MERC-008", "MERC-007", "MERC-005"]},
        "merchant": {"id": "MERC-068", "mcc": "7802", "avg_amount": 54.86},
        "terminal": {"is_online": false, "card_present": true, "km_from_home": 952.27},
        "last_transaction": null
    }')
echo "Response: $RESPONSE"
check "has approved field" "approved" "$RESPONSE"
check "has fraud_score field" "fraud_score" "$RESPONSE"

# Test 4: Invalid JSON (should not return 500)
echo ""
echo "--- Testing error handling ---"
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE_URL/fraud-score" \
    -H "Content-Type: application/json" \
    -d 'this is not json')
check "invalid JSON returns 200 (not 500)" "200" "$HTTP_CODE"

# Test 5: 404 for unknown route
echo ""
echo "--- Testing 404 ---"
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$BASE_URL/nonexistent")
check "unknown route returns 404" "404" "$HTTP_CODE"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
exit $FAIL
```

- [ ] **Step 2: Build and start the stack**

```bash
docker compose up --build -d
```

Wait for the build (may take several minutes on first run due to k-means clustering).

- [ ] **Step 3: Wait for health check and run tests**

```bash
# Wait for ready
for i in $(seq 1 30); do
    if curl -s http://localhost:9999/ready | grep -q OK; then
        echo "API is ready!"
        break
    fi
    echo "Waiting for API... ($i/30)"
    sleep 3
done

chmod +x scripts/test_integration.sh
./scripts/test_integration.sh
```

Expected: All tests PASS.

- [ ] **Step 4: Verify latency with a quick benchmark**

```bash
# Quick latency check (10 requests)
for i in $(seq 1 10); do
    curl -s -o /dev/null -w "req $i: %{time_total}s\n" -X POST http://localhost:9999/fraud-score \
        -H "Content-Type: application/json" \
        -d '{"id":"tx-bench","transaction":{"amount":100,"installments":1,"requested_at":"2026-01-01T12:00:00Z"},"customer":{"avg_amount":200,"tx_count_24h":1,"known_merchants":["MERC-001"]},"merchant":{"id":"MERC-001","mcc":"5411","avg_amount":100},"terminal":{"is_online":false,"card_present":true,"km_from_home":10},"last_transaction":null}'
done
```

- [ ] **Step 5: Stop the stack and commit**

```bash
docker compose down
git add scripts/test_integration.sh
git commit -m "test: add integration test script

Tests /ready, /fraud-score with legit and fraud payloads,
error handling (no HTTP 500), and 404 for unknown routes.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 13: Submission Branch Setup

**Files:**
- Modify: `docker-compose.yml` (submission version uses pre-built images)

- [ ] **Step 1: Push main branch to GitHub**

First create the repo on GitHub (update username as needed):

```bash
gh repo create rinha-de-backend-2026 --public --source=. --remote=origin
git push -u origin main
```

- [ ] **Step 2: Build and push Docker image**

```bash
# Build for linux/amd64
docker build --platform linux/amd64 -t ghcr.io/luizschons/rinha-backend-2026:latest .

# Login to GHCR
echo $GITHUB_TOKEN | docker login ghcr.io -u luizschons --password-stdin

# Push
docker push ghcr.io/luizschons/rinha-backend-2026:latest
```

(Adjust registry and username as needed. Docker Hub also works.)

- [ ] **Step 3: Create submission branch**

```bash
git checkout --orphan submission
git rm -rf .
```

- [ ] **Step 4: Create submission docker-compose.yml**

Create `docker-compose.yml` on submission branch (references the pre-built image):
```yaml
services:
  haproxy:
    image: haproxy:2.9-alpine
    ports:
      - "9999:9999"
    volumes:
      - ./haproxy.cfg:/usr/local/etc/haproxy/haproxy.cfg:ro
    depends_on:
      api-1:
        condition: service_started
      api-2:
        condition: service_started
    deploy:
      resources:
        limits:
          cpus: "0.05"
          memory: "10MB"
    networks:
      - rinha

  api-1:
    image: ghcr.io/luizschons/rinha-backend-2026:latest
    platform: linux/amd64
    environment:
      - NPROBE=10
      - WORKERS=2
    deploy:
      resources:
        limits:
          cpus: "0.475"
          memory: "170MB"
    networks:
      - rinha

  api-2:
    image: ghcr.io/luizschons/rinha-backend-2026:latest
    platform: linux/amd64
    environment:
      - NPROBE=10
      - WORKERS=2
    deploy:
      resources:
        limits:
          cpus: "0.475"
          memory: "170MB"
    networks:
      - rinha

networks:
  rinha:
    driver: bridge
```

- [ ] **Step 5: Copy HAProxy config and info.json to submission branch**

```bash
# From main branch, get the files we need
git checkout main -- config/haproxy.cfg info.json
mv config/haproxy.cfg ./haproxy.cfg
rm -rf config
```

- [ ] **Step 6: Commit and push submission branch**

```bash
git add docker-compose.yml haproxy.cfg info.json
git commit -m "chore: submission branch with docker-compose

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
git push -u origin submission
git checkout main
```

- [ ] **Step 7: Register as participant**

Open a PR on the competition repo adding `participants/<your-username>.json`:
```json
[{
    "id": "php-swoole",
    "repo": "https://github.com/luizschons/rinha-de-backend-2026"
}]
```

---

## Tuning Parameters (post-implementation)

After all tasks are complete and integration tests pass, these parameters can be tuned via environment variables or rebuild:

| Parameter | Default | Env Var | Effect |
|-----------|---------|---------|--------|
| Clusters | 1500 | `N_CLUSTERS` (build arg) | More = faster queries, risk lower accuracy |
| Probes | 10 | `NPROBE` | More = better accuracy, slower queries |
| Workers | 2 | `WORKERS` | More = handle concurrency, risk context switching |
| CPU per API | 0.475 | docker-compose | Balance between API instances |
| Memory per API | 170MB | docker-compose | Must fit PHP + mmap pages |

**Tuning workflow:**
1. Build and push new image
2. Open issue with `rinha/test` on competition repo
3. Check preview results
4. Adjust parameters and repeat
