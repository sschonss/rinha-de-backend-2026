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
                if (g_idx.labels[base + s] == 0xFF) continue;  // padding sentinel
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

// ============================================================
// In-C JSON parser + scoring (eliminates PHP per-request overhead)
// ============================================================

#include <ctype.h>

// Find first occurrence of "key": within [s, e). Returns ptr to value's first char,
// or NULL. Skips whitespace/colon between key and value. Exact key match (boundary on quotes).
static const char *jp_find_key(const char *s, const char *e, const char *key, size_t klen) {
    if (!s || !e || s >= e) return NULL;
    const char *limit = e - klen - 2;
    for (; s < limit; s++) {
        if (*s != '"') continue;
        if (memcmp(s + 1, key, klen) != 0) continue;
        if (s[klen + 1] != '"') continue;
        const char *p = s + klen + 2;
        while (p < e && (*p == ':' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        return p;
    }
    return NULL;
}

// Find object value bounds for "key": { ... }. Returns 1 on success.
static int jp_find_obj(const char *s, const char *e, const char *key, size_t klen,
                       const char **out_start, const char **out_end) {
    const char *p = jp_find_key(s, e, key, klen);
    if (!p || *p != '{') return 0;
    int depth = 0;
    const char *q = p;
    for (; q < e; q++) {
        if (*q == '"') {
            q++;
            while (q < e && *q != '"') { if (*q == '\\' && q + 1 < e) q++; q++; }
        } else if (*q == '{') depth++;
        else if (*q == '}') { if (--depth == 0) { q++; break; } }
    }
    *out_start = p;
    *out_end = q;
    return 1;
}

// Find array value bounds for "key": [ ... ]. Returns 1 on success.
static int jp_find_arr(const char *s, const char *e, const char *key, size_t klen,
                       const char **out_start, const char **out_end) {
    const char *p = jp_find_key(s, e, key, klen);
    if (!p || *p != '[') return 0;
    int depth = 0;
    const char *q = p;
    for (; q < e; q++) {
        if (*q == '"') {
            q++;
            while (q < e && *q != '"') { if (*q == '\\' && q + 1 < e) q++; q++; }
        } else if (*q == '[') depth++;
        else if (*q == ']') { if (--depth == 0) { q++; break; } }
    }
    *out_start = p;
    *out_end = q;
    return 1;
}

static double mcc_risk_lookup_q(const char *p) {
    // p points to opening quote of "NNNN"
    if (!p || *p != '"') return 0.5;
    const char *s = p + 1;
    int mcc = 0;
    for (int i = 0; i < 6 && s[i] >= '0' && s[i] <= '9'; i++) mcc = mcc * 10 + (s[i] - '0');
    switch (mcc) {
        case 5411: return 0.15;
        case 5812: return 0.30;
        case 5912: return 0.20;
        case 5944: return 0.45;
        case 7801: return 0.80;
        case 7802: return 0.75;
        case 7995: return 0.85;
        case 4511: return 0.35;
        case 5311: return 0.25;
        case 5999: return 0.50;
        default:   return 0.50;
    }
}

// Parse "YYYY-MM-DDTHH:MM:SSZ" -> epoch seconds (UTC).
static int64_t parse_iso8601_z(const char *s) {
    int y  = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    int mo = (s[5]-'0')*10 + (s[6]-'0');
    int d  = (s[8]-'0')*10 + (s[9]-'0');
    int h  = (s[11]-'0')*10 + (s[12]-'0');
    int mi = (s[14]-'0')*10 + (s[15]-'0');
    int se = (s[17]-'0')*10 + (s[18]-'0');
    // Howard Hinnant's days_from_civil
    int yy = y - (mo <= 2);
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (153u * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return days * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 + (int64_t)se;
}

// Find merchant.id within known_merchants array. Returns 1 if NOT found (unknown).
static int jp_unknown_merchant(const char *mid, size_t mid_len,
                                const char *arr_s, const char *arr_e) {
    if (!mid || mid_len == 0 || !arr_s || arr_s + 1 >= arr_e) return 1;
    const char *limit = arr_e - mid_len - 1;
    for (const char *p = arr_s + 1; p < limit; p++) {
        if (*p == '"' && memcmp(p + 1, mid, mid_len) == 0 && p[1 + mid_len] == '"') {
            return 0;
        }
    }
    return 1;
}

int ivf_score_json(const char *json, int len) {
    if (!g_idx.centroids || !json || len < 50) return 0;
    const char *s = json;
    const char *e = json + len;

    const char *tx_s = NULL, *tx_e = NULL;
    const char *cust_s = NULL, *cust_e = NULL;
    const char *mer_s = NULL, *mer_e = NULL;
    const char *term_s = NULL, *term_e = NULL;

    if (!jp_find_obj(s, e, "transaction", 11, &tx_s, &tx_e)) return 0;
    if (!jp_find_obj(s, e, "customer",     8, &cust_s, &cust_e)) return 0;
    if (!jp_find_obj(s, e, "merchant",     8, &mer_s, &mer_e)) return 0;
    if (!jp_find_obj(s, e, "terminal",     8, &term_s, &term_e)) return 0;

    // transaction
    const char *p_amt = jp_find_key(tx_s, tx_e, "amount", 6);
    float amount = p_amt ? (float)strtod(p_amt, NULL) : 0.0f;

    const char *p_inst = jp_find_key(tx_s, tx_e, "installments", 12);
    int installments = p_inst ? (int)strtol(p_inst, NULL, 10) : 1;

    const char *p_req = jp_find_key(tx_s, tx_e, "requested_at", 12);
    if (!p_req || *p_req != '"') return 0;
    int64_t cur_ts = parse_iso8601_z(p_req + 1);
    int hour = (int)(((cur_ts % 86400) + 86400) % 86400) / 3600;
    int64_t days_floor = cur_ts >= 0 ? cur_ts / 86400 : -((-cur_ts + 86399) / 86400);
    // 1970-01-01 = Thursday. PHP gmdate('N')-1 gives Mon=0..Sun=6
    int dow = (int)(((days_floor + 3) % 7 + 7) % 7);

    // customer
    const char *p_cavg = jp_find_key(cust_s, cust_e, "avg_amount", 10);
    float cust_avg = p_cavg ? (float)strtod(p_cavg, NULL) : 0.0f;

    const char *p_tx24 = jp_find_key(cust_s, cust_e, "tx_count_24h", 12);
    int tx_count_24h = p_tx24 ? (int)strtol(p_tx24, NULL, 10) : 0;

    const char *km_s = NULL, *km_e = NULL;
    jp_find_arr(cust_s, cust_e, "known_merchants", 15, &km_s, &km_e);

    // merchant
    const char *p_mid = jp_find_key(mer_s, mer_e, "id", 2);
    const char *mid_str = NULL; size_t mid_len = 0;
    if (p_mid && *p_mid == '"') {
        mid_str = p_mid + 1;
        const char *q = mid_str;
        while (q < mer_e && *q != '"') q++;
        mid_len = (size_t)(q - mid_str);
    }
    const char *p_mcc = jp_find_key(mer_s, mer_e, "mcc", 3);
    double mcc_risk = mcc_risk_lookup_q(p_mcc);
    const char *p_mavg = jp_find_key(mer_s, mer_e, "avg_amount", 10);
    float merch_avg = p_mavg ? (float)strtod(p_mavg, NULL) : 0.0f;
    int unknown = (km_s && mid_str) ? jp_unknown_merchant(mid_str, mid_len, km_s, km_e) : 1;

    // terminal
    const char *p_io = jp_find_key(term_s, term_e, "is_online", 9);
    int is_online = (p_io && *p_io == 't') ? 1 : 0;
    const char *p_cp = jp_find_key(term_s, term_e, "card_present", 12);
    int card_present = (p_cp && *p_cp == 't') ? 1 : 0;
    const char *p_kmh = jp_find_key(term_s, term_e, "km_from_home", 12);
    float km_home = p_kmh ? (float)strtod(p_kmh, NULL) : 0.0f;

    // last_transaction (object or null)
    int has_last = 0;
    float minutes_since_last = 0.0f, km_from_last = 0.0f;
    const char *p_lt = jp_find_key(s, e, "last_transaction", 16);
    if (p_lt && *p_lt == '{') {
        const char *lt_s, *lt_e;
        if (jp_find_obj(s, e, "last_transaction", 16, &lt_s, &lt_e)) {
            const char *p_ts = jp_find_key(lt_s, lt_e, "timestamp", 9);
            const char *p_kf = jp_find_key(lt_s, lt_e, "km_from_current", 15);
            if (p_ts && *p_ts == '"' && p_kf) {
                int64_t last_ts = parse_iso8601_z(p_ts + 1);
                minutes_since_last = (float)((double)(cur_ts - last_ts) / 60.0);
                km_from_last = (float)strtod(p_kf, NULL);
                has_last = 1;
            }
        }
    }

    return ivf_fraud_score(
        amount, installments, cust_avg,
        tx_count_24h, merch_avg, (float)mcc_risk,
        km_home, is_online, card_present, unknown,
        hour, dow, has_last, minutes_since_last, km_from_last);
}
