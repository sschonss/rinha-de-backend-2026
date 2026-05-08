// IVF1 index reader + quantized KNN search (int16, AVX2+FMA).
//
// File layout (see indexer/build_index.py):
//   magic "IVF1" | u32 n | u32 k | u32 d | u32 total_blocks
//   centroids[k][d]  (f32)
//   block_offsets[k+1] (u32, in blocks)
//   labels[total_blocks*8] (u8)
//   blocks[total_blocks][d][8] (i16, dim-major within each block)
//
// All arrays are loaded into the C heap (malloc + fread). No PHP intermediary.

#include "vector_search.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <immintrin.h>

#define DIMS 14
#define BLOCK_VECS 8                  // 8 quantized vectors per block
#define BLOCK_BYTES (DIMS*BLOCK_VECS*2)
#define QUANT_SCALE 10000.0f
#define PAD_SENTINEL_F  32767.0f
#define MAX_NPROBE 64
#define K_NEIGHBORS 5

typedef struct {
    float    *centroids;     // [k * DIMS]
    uint32_t *offsets;       // [k + 1]
    uint8_t  *labels;        // [total_blocks * 8]
    int16_t  *blocks;        // [total_blocks * DIMS * 8]
    uint32_t  n_vectors;
    uint32_t  n_clusters;
    uint32_t  n_dims;
    uint32_t  total_blocks;
    int       fast_nprobe;
    int       full_nprobe;
} IVFIndex;

static IVFIndex g_idx;

// ---------- file load ----------

static int read_exact(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int ivf_init(const char *index_path, int fast_nprobe, int full_nprobe) {
    memset(&g_idx, 0, sizeof(g_idx));
    g_idx.fast_nprobe = fast_nprobe > 0 ? fast_nprobe : 8;
    g_idx.full_nprobe = full_nprobe > 0 ? full_nprobe : 24;
    if (g_idx.fast_nprobe > MAX_NPROBE) g_idx.fast_nprobe = MAX_NPROBE;
    if (g_idx.full_nprobe > MAX_NPROBE) g_idx.full_nprobe = MAX_NPROBE;

    int fd = open(index_path, O_RDONLY);
    if (fd < 0) { perror(index_path); return -1; }

    char magic[4];
    if (read_exact(fd, magic, 4) < 0) { close(fd); return -1; }
    if (memcmp(magic, "IVF1", 4) != 0) {
        fprintf(stderr, "[ivf] bad magic\n");
        close(fd);
        return -1;
    }

    uint32_t hdr[4];
    if (read_exact(fd, hdr, sizeof(hdr)) < 0) { close(fd); return -1; }
    g_idx.n_vectors    = hdr[0];
    g_idx.n_clusters   = hdr[1];
    g_idx.n_dims       = hdr[2];
    g_idx.total_blocks = hdr[3];

    if (g_idx.n_dims != DIMS) {
        fprintf(stderr, "[ivf] expected %d dims, got %u\n", DIMS, g_idx.n_dims);
        close(fd);
        return -1;
    }

    fprintf(stderr, "[ivf] n=%u k=%u d=%u total_blocks=%u fast=%d full=%d\n",
            g_idx.n_vectors, g_idx.n_clusters, g_idx.n_dims, g_idx.total_blocks,
            g_idx.fast_nprobe, g_idx.full_nprobe);

    size_t cent_bytes   = (size_t)g_idx.n_clusters * DIMS * sizeof(float);
    size_t off_bytes    = (size_t)(g_idx.n_clusters + 1) * sizeof(uint32_t);
    size_t labels_bytes = (size_t)g_idx.total_blocks * BLOCK_VECS;
    size_t blocks_bytes = (size_t)g_idx.total_blocks * BLOCK_BYTES;

    if (posix_memalign((void **)&g_idx.centroids, 32, cent_bytes) != 0) goto fail;
    if (posix_memalign((void **)&g_idx.offsets,   32, off_bytes)  != 0) goto fail;
    if (posix_memalign((void **)&g_idx.labels,    32, labels_bytes)!= 0) goto fail;
    if (posix_memalign((void **)&g_idx.blocks,    32, blocks_bytes)!= 0) goto fail;

    if (read_exact(fd, g_idx.centroids, cent_bytes) < 0) goto fail;
    if (read_exact(fd, g_idx.offsets,   off_bytes)  < 0) goto fail;
    if (read_exact(fd, g_idx.labels,    labels_bytes)< 0) goto fail;
    if (read_exact(fd, g_idx.blocks,    blocks_bytes)< 0) goto fail;
    close(fd);

    fprintf(stderr, "[ivf] index loaded (%.1f MB heap)\n",
            (cent_bytes + off_bytes + labels_bytes + blocks_bytes) / (1024.0*1024.0));
    return 0;

fail:
    fprintf(stderr, "[ivf] load failed\n");
    if (fd >= 0) close(fd);
    free(g_idx.centroids); free(g_idx.offsets);
    free(g_idx.labels);    free(g_idx.blocks);
    memset(&g_idx, 0, sizeof(g_idx));
    return -1;
}

void ivf_destroy(void) {
    free(g_idx.centroids); free(g_idx.offsets);
    free(g_idx.labels);    free(g_idx.blocks);
    memset(&g_idx, 0, sizeof(g_idx));
}

// ---------- centroid distance (AVX2+FMA, f32, 14 dims) ----------

static inline float dist_centroid(const float *restrict q, const float *restrict c) {
    // dims 0..7
    __m256 qa = _mm256_loadu_ps(q);
    __m256 ca = _mm256_loadu_ps(c);
    __m256 da = _mm256_sub_ps(qa, ca);
    __m256 acc = _mm256_mul_ps(da, da);
    // dims 8..13 -> use 4-wide + scalar tail
    __m128 qb = _mm_loadu_ps(q + 8);
    __m128 cb = _mm_loadu_ps(c + 8);
    __m128 db = _mm_sub_ps(qb, cb);
    __m128 sb = _mm_mul_ps(db, db);

    __m128 lo = _mm256_castps256_ps128(acc);
    __m128 hi = _mm256_extractf128_ps(acc, 1);
    __m128 sum = _mm_add_ps(_mm_add_ps(lo, hi), sb);
    __m128 shuf = _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(2,3,0,1));
    __m128 sums = _mm_add_ps(sum, shuf);
    shuf = _mm_movehl_ps(shuf, sums);
    sums = _mm_add_ss(sums, shuf);

    float s;
    _mm_store_ss(&s, sums);

    float d12 = q[12] - c[12], d13 = q[13] - c[13];
    return s + d12*d12 + d13*d13;
}

// ---------- top-N centroids (sorted insertion) ----------

static int top_n_centroids(const float *q, int nprobe, int *out_ids) {
    float best_d[MAX_NPROBE];
    int n = 0;
    uint32_t k = g_idx.n_clusters;
    for (uint32_t c = 0; c < k; c++) {
        float d = dist_centroid(q, g_idx.centroids + (size_t)c * DIMS);
        if (n < nprobe) {
            int pos = n;
            while (pos > 0 && d < best_d[pos - 1]) {
                best_d[pos] = best_d[pos - 1];
                out_ids[pos] = out_ids[pos - 1];
                pos--;
            }
            best_d[pos] = d; out_ids[pos] = (int)c;
            n++;
        } else if (d < best_d[n - 1]) {
            int pos = n - 1;
            while (pos > 0 && d < best_d[pos - 1]) {
                best_d[pos] = best_d[pos - 1];
                out_ids[pos] = out_ids[pos - 1];
                pos--;
            }
            best_d[pos] = d; out_ids[pos] = (int)c;
        }
    }
    return n;
}

// ---------- block scan with int16 distances + early-exit ----------

typedef struct { int32_t dist; uint32_t slot; } HeapItem;

static inline void heap_sift_down(HeapItem *h, int n, int i) {
    for (;;) {
        int l = 2*i + 1, r = 2*i + 2, m = i;
        if (l < n && h[l].dist > h[m].dist) m = l;
        if (r < n && h[r].dist > h[m].dist) m = r;
        if (m == i) return;
        HeapItem t = h[i]; h[i] = h[m]; h[m] = t;
        i = m;
    }
}

static inline void heap_push(HeapItem *h, int *n, int max_n, int32_t d, uint32_t slot) {
    if (*n < max_n) {
        h[*n].dist = d; h[*n].slot = slot;
        (*n)++;
        int i = *n - 1;
        while (i > 0) {
            int p = (i - 1) / 2;
            if (h[p].dist < h[i].dist) {
                HeapItem t = h[p]; h[p] = h[i]; h[i] = t;
                i = p;
            } else break;
        }
    } else if (d < h[0].dist) {
        h[0].dist = d; h[0].slot = slot;
        heap_sift_down(h, max_n, 0);
    }
}

// Process one block: 8 vectors, 14 dims, dim-major (block[d*8 + slot]).
// Accumulates squared distance into 8 i32 lanes.
// Early-exit after 8 dims if all 8 lanes >= worst_threshold (heap top).
static inline void scan_block(
    const __m256i *restrict q_lanes,   // 14 vectors: each is 8x q[d] broadcast as i32
    const int16_t *restrict block,
    int32_t worst,
    int do_early_exit,
    int32_t out_dists[8])
{
    __m256i acc = _mm256_setzero_si256();

    // First 8 dims
    for (int d = 0; d < 8; d++) {
        __m128i v16 = _mm_loadu_si128((const __m128i *)(block + d*8));
        __m256i v32 = _mm256_cvtepi16_epi32(v16);
        __m256i diff = _mm256_sub_epi32(v32, q_lanes[d]);
        acc = _mm256_add_epi32(acc, _mm256_mullo_epi32(diff, diff));
    }

    if (do_early_exit) {
        // worst broadcast
        __m256i wv = _mm256_set1_epi32(worst);
        // mask: 1 where acc < worst (still candidate)
        __m256i lt = _mm256_cmpgt_epi32(wv, acc);
        if (_mm256_testz_si256(lt, lt)) {
            // all 8 lanes already >= worst — skip remaining dims
            for (int i = 0; i < 8; i++) out_dists[i] = INT32_MAX;
            return;
        }
    }

    // Remaining 6 dims
    for (int d = 8; d < DIMS; d++) {
        __m128i v16 = _mm_loadu_si128((const __m128i *)(block + d*8));
        __m256i v32 = _mm256_cvtepi16_epi32(v16);
        __m256i diff = _mm256_sub_epi32(v32, q_lanes[d]);
        acc = _mm256_add_epi32(acc, _mm256_mullo_epi32(diff, diff));
    }

    _mm256_storeu_si256((__m256i *)out_dists, acc);
}

// ---------- KNN over a set of clusters ----------

static int knn_count_fraud(const __m256i *q_lanes, const int *probe_ids, int nprobe) {
    HeapItem heap[K_NEIGHBORS];
    int hn = 0;
    int32_t worst = INT32_MAX;

    for (int p = 0; p < nprobe; p++) {
        int c = probe_ids[p];
        uint32_t b0 = g_idx.offsets[c];
        uint32_t b1 = g_idx.offsets[c + 1];

        // Prefetch the next cluster's first block while scanning this one.
        if (p + 1 < nprobe) {
            uint32_t nb = g_idx.offsets[probe_ids[p + 1]];
            _mm_prefetch((const char *)(g_idx.blocks + (size_t)nb * DIMS * 8), _MM_HINT_T0);
        }

        for (uint32_t b = b0; b < b1; b++) {
            // Prefetch a few blocks ahead within the same cluster
            if (b + 4 < b1) {
                _mm_prefetch((const char *)(g_idx.blocks + (size_t)(b + 4) * DIMS * 8),
                             _MM_HINT_T0);
            }
            int32_t dists[8];
            scan_block(q_lanes, g_idx.blocks + (size_t)b * DIMS * 8,
                       worst, hn == K_NEIGHBORS, dists);
            uint32_t base = b * BLOCK_VECS;
            for (int s = 0; s < BLOCK_VECS; s++) {
                if (dists[s] == INT32_MAX) continue;
                heap_push(heap, &hn, K_NEIGHBORS, dists[s], base + s);
                if (hn == K_NEIGHBORS) worst = heap[0].dist;
            }
        }
    }

    int fraud = 0;
    for (int i = 0; i < hn; i++) fraud += g_idx.labels[heap[i].slot];
    return fraud;
}

// ---------- public entry: vectorize + quantize + KNN ----------

static inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

static inline int16_t quantize_one(float v) {
    int x = (int)lrintf(v * QUANT_SCALE);
    if (x < -32768) x = -32768;
    if (x >  32767) x =  32767;
    return (int16_t)x;
}

static void build_query_vec(
    float amount, int installments, float cust_avg,
    int tx_count_24h, float merch_avg, float mcc_risk,
    float km_home, int is_online, int card_present,
    int unknown_merchant, int hour, int dow,
    int has_last_tx, float minutes_since_last, float km_from_last,
    float qf[DIMS], int16_t qi[DIMS])
{
    qf[0]  = clamp01(amount / 10000.0f);
    qf[1]  = clamp01((float)installments / 12.0f);
    qf[2]  = clamp01(cust_avg > 0.0f ? amount / (cust_avg * 10.0f) : 1.0f);
    qf[3]  = (float)hour / 23.0f;
    qf[4]  = (float)dow  / 6.0f;
    if (has_last_tx) {
        qf[5] = clamp01(minutes_since_last / 1440.0f);
        qf[6] = clamp01(km_from_last / 1000.0f);
    } else {
        qf[5] = -1.0f;
        qf[6] = -1.0f;
    }
    qf[7]  = clamp01(km_home / 1000.0f);
    qf[8]  = clamp01((float)tx_count_24h / 20.0f);
    qf[9]  = is_online    ? 1.0f : 0.0f;
    qf[10] = card_present ? 1.0f : 0.0f;
    qf[11] = unknown_merchant ? 1.0f : 0.0f;
    qf[12] = mcc_risk;
    qf[13] = clamp01(merch_avg / 10000.0f);
    for (int i = 0; i < DIMS; i++) qi[i] = quantize_one(qf[i]);
}

int ivf_fraud_score(
    float amount, int installments, float cust_avg,
    int tx_count_24h, float merch_avg, float mcc_risk,
    float km_home, int is_online, int card_present,
    int unknown_merchant, int hour, int dow,
    int has_last_tx, float minutes_since_last, float km_from_last)
{
    if (!g_idx.centroids) return 0;

    float qf[DIMS];
    int16_t qi[DIMS];
    build_query_vec(amount, installments, cust_avg, tx_count_24h, merch_avg, mcc_risk,
                    km_home, is_online, card_present, unknown_merchant, hour, dow,
                    has_last_tx, minutes_since_last, km_from_last, qf, qi);

    // Pre-broadcast each q dim into 8x i32 lanes (used by every block scan).
    __m256i q_lanes[DIMS];
    for (int d = 0; d < DIMS; d++) q_lanes[d] = _mm256_set1_epi32((int32_t)qi[d]);

    // Fast pass
    int probes[MAX_NPROBE];
    int n = top_n_centroids(qf, g_idx.fast_nprobe, probes);
    int fast = knn_count_fraud(q_lanes, probes, n);

    // Boundary fallback: if count is ambiguous (2 or 3), reprobe with full nprobe
    if ((fast == 2 || fast == 3) && g_idx.full_nprobe > g_idx.fast_nprobe) {
        n = top_n_centroids(qf, g_idx.full_nprobe, probes);
        return knn_count_fraud(q_lanes, probes, n);
    }
    return fast;
}

// ---------- pre-warm ----------

int ivf_warmup(int n_queries) {
    if (!g_idx.centroids) return -1;
    // Deterministic pseudo-random seed mix; values stay in clamp/sentinel space.
    unsigned s = 0x9E3779B9u;
    int sink = 0;
    for (int i = 0; i < n_queries; i++) {
        s = s * 1664525u + 1013904223u;
        float a = (float)((s >> 8) & 0xFFFF) / 65535.0f * 5000.0f;
        s = s * 1664525u + 1013904223u;
        float ca = (float)((s >> 8) & 0xFFFF) / 65535.0f * 1000.0f;
        s = s * 1664525u + 1013904223u;
        float ma = (float)((s >> 8) & 0xFFFF) / 65535.0f * 1000.0f;
        s = s * 1664525u + 1013904223u;
        float kh = (float)((s >> 8) & 0xFFFF) / 65535.0f * 500.0f;
        s = s * 1664525u + 1013904223u;
        int   tc = (int)((s >> 8) & 31);
        s = s * 1664525u + 1013904223u;
        int   io = (int)(s & 1);
        s = s * 1664525u + 1013904223u;
        int   cp = (int)(s & 1);
        s = s * 1664525u + 1013904223u;
        int   um = (int)(s & 1);
        s = s * 1664525u + 1013904223u;
        int   hr = (int)((s >> 8) % 24);
        s = s * 1664525u + 1013904223u;
        int   dw = (int)((s >> 8) % 7);
        s = s * 1664525u + 1013904223u;
        int   hl = (int)(s & 1);
        s = s * 1664525u + 1013904223u;
        float ms = (float)((s >> 8) & 0xFFFF) / 65535.0f * 2000.0f;
        s = s * 1664525u + 1013904223u;
        float kl = (float)((s >> 8) & 0xFFFF) / 65535.0f * 500.0f;
        sink += ivf_fraud_score(a, (int)((s>>3)%12), ca, tc, ma, 0.5f,
                                kh, io, cp, um, hr, dw, hl, ms, kl);
    }
    return sink;  // prevent dead-code elimination
}
