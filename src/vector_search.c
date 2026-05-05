#include "vector_search.h"
#include "yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <immintrin.h>

typedef struct {
    float    *centroids;
    float    *vectors;
    uint8_t  *labels;
    uint32_t *offsets;
    uint32_t  n_vectors;
    uint32_t  n_clusters;
    uint32_t  n_dims;
    int       nprobe;
    void   *mmap_ptrs[5];
    size_t  mmap_sizes[5];
    int     n_mmaps;
} IVFIndex;

static IVFIndex g_idx;

static void *mmap_file(const char *path, size_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return NULL; }
    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return NULL; }
    *out_size = (size_t)st.st_size;
    void *ptr = mmap(NULL, *out_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) { perror("mmap"); return NULL; }
    return ptr;
}

// AVX2 euclidean distance for 14 dimensions (no sqrt needed)
static inline float dist_euclidean(const float *restrict a, const float *restrict b) {
    // AVX2: dims 0-7 (8 floats at once)
    __m256 va = _mm256_loadu_ps(a);
    __m256 vb = _mm256_loadu_ps(b);
    __m256 d = _mm256_sub_ps(va, vb);
    __m256 sq = _mm256_mul_ps(d, d);

    // SSE: dims 8-11 (4 floats)
    __m128 va2 = _mm_loadu_ps(a + 8);
    __m128 vb2 = _mm_loadu_ps(b + 8);
    __m128 d2 = _mm_sub_ps(va2, vb2);
    __m128 sq2 = _mm_mul_ps(d2, d2);

    // Reduce AVX2 → SSE and combine
    __m128 lo = _mm256_castps256_ps128(sq);
    __m128 hi = _mm256_extractf128_ps(sq, 1);
    __m128 sum = _mm_add_ps(_mm_add_ps(lo, hi), sq2);

    // Horizontal sum (4 floats → 1)
    __m128 shuf = _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(2, 3, 0, 1));
    __m128 sums = _mm_add_ps(sum, shuf);
    shuf = _mm_movehl_ps(shuf, sums);
    sums = _mm_add_ss(sums, shuf);

    float s;
    _mm_store_ss(&s, sums);

    // Scalar: dims 12-13
    float d12 = a[12] - b[12], d13 = a[13] - b[13];
    return s + d12 * d12 + d13 * d13;
}

typedef struct { float dist; uint32_t idx; } HeapItem;

static inline void heap_sift_down(HeapItem *heap, int size, int i) {
    while (1) {
        int largest = i, left = 2*i+1, right = 2*i+2;
        if (left < size && heap[left].dist > heap[largest].dist) largest = left;
        if (right < size && heap[right].dist > heap[largest].dist) largest = right;
        if (largest == i) break;
        HeapItem tmp = heap[i]; heap[i] = heap[largest]; heap[largest] = tmp;
        i = largest;
    }
}

static inline void heap_push(HeapItem *heap, int *size, int max_size, float dist, uint32_t idx) {
    if (*size < max_size) {
        heap[*size].dist = dist; heap[*size].idx = idx;
        (*size)++;
        // Sift up
        int i = *size - 1;
        while (i > 0) {
            int parent = (i-1)/2;
            if (heap[parent].dist < heap[i].dist) {
                HeapItem tmp = heap[parent]; heap[parent] = heap[i]; heap[i] = tmp;
                i = parent;
            } else break;
        }
    } else if (dist < heap[0].dist) {
        heap[0].dist = dist; heap[0].idx = idx;
        heap_sift_down(heap, max_size, 0);
    }
}

int ivf_init(const char *index_dir, int nprobe) {
    char path[512];
    memset(&g_idx, 0, sizeof(g_idx));
    g_idx.nprobe = nprobe > 0 ? nprobe : 10;

    snprintf(path, sizeof(path), "%s/meta.bin", index_dir);
    size_t sz;
    uint32_t *meta = (uint32_t *)mmap_file(path, &sz);
    if (!meta) return -1;
    g_idx.n_vectors = meta[0]; g_idx.n_clusters = meta[1]; g_idx.n_dims = meta[2];
    g_idx.mmap_ptrs[g_idx.n_mmaps] = meta; g_idx.mmap_sizes[g_idx.n_mmaps] = sz; g_idx.n_mmaps++;

    fprintf(stderr, "[ivf] n_vectors=%u, n_clusters=%u, n_dims=%u, nprobe=%d\n",
            g_idx.n_vectors, g_idx.n_clusters, g_idx.n_dims, g_idx.nprobe);

    snprintf(path, sizeof(path), "%s/centroids.bin", index_dir);
    g_idx.centroids = (float *)mmap_file(path, &sz);
    if (!g_idx.centroids) return -1;
    g_idx.mmap_ptrs[g_idx.n_mmaps] = g_idx.centroids; g_idx.mmap_sizes[g_idx.n_mmaps] = sz; g_idx.n_mmaps++;

    snprintf(path, sizeof(path), "%s/vectors.bin", index_dir);
    g_idx.vectors = (float *)mmap_file(path, &sz);
    if (!g_idx.vectors) return -1;
    madvise(g_idx.vectors, sz, MADV_RANDOM);
    g_idx.mmap_ptrs[g_idx.n_mmaps] = g_idx.vectors; g_idx.mmap_sizes[g_idx.n_mmaps] = sz; g_idx.n_mmaps++;

    snprintf(path, sizeof(path), "%s/labels.bin", index_dir);
    g_idx.labels = (uint8_t *)mmap_file(path, &sz);
    if (!g_idx.labels) return -1;
    madvise(g_idx.labels, sz, MADV_RANDOM);
    g_idx.mmap_ptrs[g_idx.n_mmaps] = g_idx.labels; g_idx.mmap_sizes[g_idx.n_mmaps] = sz; g_idx.n_mmaps++;

    snprintf(path, sizeof(path), "%s/offsets.bin", index_dir);
    g_idx.offsets = (uint32_t *)mmap_file(path, &sz);
    if (!g_idx.offsets) return -1;
    g_idx.mmap_ptrs[g_idx.n_mmaps] = g_idx.offsets; g_idx.mmap_sizes[g_idx.n_mmaps] = sz; g_idx.n_mmaps++;

    fprintf(stderr, "[ivf] index loaded successfully\n");
    return 0;
}

int ivf_search(const float *query, int *out_labels, float *out_distances, int k) {
    if (!g_idx.centroids) return -1;
    int nprobe = g_idx.nprobe;
    uint32_t n_clusters = g_idx.n_clusters;

    // Find top nprobe nearest centroids (sorted insertion)
    float cent_dists[64]; int cent_ids[64]; int n_found = 0;
    if (nprobe > 64) nprobe = 64;

    for (uint32_t c = 0; c < n_clusters; c++) {
        float d = dist_euclidean(query, g_idx.centroids + c * IVF_DIMS);
        if (n_found < nprobe) {
            int pos = n_found;
            while (pos > 0 && d < cent_dists[pos-1]) {
                cent_dists[pos] = cent_dists[pos-1]; cent_ids[pos] = cent_ids[pos-1]; pos--;
            }
            cent_dists[pos] = d; cent_ids[pos] = c; n_found++;
        } else if (d < cent_dists[n_found-1]) {
            int pos = n_found - 1;
            while (pos > 0 && d < cent_dists[pos-1]) {
                cent_dists[pos] = cent_dists[pos-1]; cent_ids[pos] = cent_ids[pos-1]; pos--;
            }
            cent_dists[pos] = d; cent_ids[pos] = c;
        }
    }

    // Search vectors in top nprobe clusters using max-heap for KNN
    HeapItem heap[IVF_K]; int heap_size = 0;
    for (int p = 0; p < nprobe; p++) {
        int c = cent_ids[p];
        uint32_t start = g_idx.offsets[c * 2];
        uint32_t count = g_idx.offsets[c * 2 + 1];
        for (uint32_t i = 0; i < count; i++) {
            uint32_t vidx = start + i;
            float d = dist_euclidean(query, g_idx.vectors + vidx * IVF_DIMS);
            heap_push(heap, &heap_size, k, d, vidx);
        }
    }

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

// Combined vectorize + search + count: eliminates PHP overhead entirely
int ivf_fraud_score(
    float amount, int installments, float cust_avg,
    int tx_count_24h, float merch_avg, float mcc_risk,
    float km_home, int is_online, int card_present,
    int unknown_merchant, int hour, int dow,
    int has_last_tx, float minutes_since_last, float km_from_last)
{
    if (!g_idx.centroids) return 0;

    #define CLAMP01(x) ((x) < 0.0f ? 0.0f : ((x) > 1.0f ? 1.0f : (x)))

    float query[16] __attribute__((aligned(32)));
    query[0]  = CLAMP01(amount / 10000.0f);
    query[1]  = CLAMP01((float)installments / 12.0f);
    query[2]  = CLAMP01(cust_avg > 0.0f ? amount / (cust_avg * 10.0f) : 1.0f);
    query[3]  = (float)hour / 23.0f;
    query[4]  = (float)dow / 6.0f;
    if (has_last_tx) {
        query[5] = CLAMP01(minutes_since_last / 1440.0f);
        query[6] = CLAMP01(km_from_last / 1000.0f);
    } else {
        query[5] = -1.0f;
        query[6] = -1.0f;
    }
    query[7]  = CLAMP01(km_home / 1000.0f);
    query[8]  = CLAMP01((float)tx_count_24h / 20.0f);
    query[9]  = is_online ? 1.0f : 0.0f;
    query[10] = card_present ? 1.0f : 0.0f;
    query[11] = unknown_merchant ? 1.0f : 0.0f;
    query[12] = mcc_risk;
    query[13] = CLAMP01(merch_avg / 10000.0f);
    query[14] = 0.0f;
    query[15] = 0.0f;

    #undef CLAMP01

    int labels[5];
    ivf_search(query, labels, NULL, 5);
    return labels[0] + labels[1] + labels[2] + labels[3] + labels[4];
}

// Safe number getter: handles both int and real JSON numbers
static inline double get_num(yyjson_val *val) {
    if (!val) return 0.0;
    if (yyjson_is_real(val)) return yyjson_get_real(val);
    if (yyjson_is_int(val)) return (double)yyjson_get_sint(val);
    return 0.0;
}

// MCC risk lookup table
typedef struct { const char *mcc; float risk; } MccEntry;
static const MccEntry MCC_TABLE[] = {
    {"4511", 0.35f}, {"5311", 0.25f}, {"5411", 0.15f}, {"5812", 0.30f},
    {"5912", 0.20f}, {"5944", 0.45f}, {"5999", 0.50f}, {"7801", 0.80f},
    {"7802", 0.75f}, {"7995", 0.85f},
};
#define MCC_TABLE_SIZE 10

static float mcc_risk_lookup(const char *mcc, size_t len) {
    for (int i = 0; i < MCC_TABLE_SIZE; i++) {
        if (len == 4 && memcmp(mcc, MCC_TABLE[i].mcc, 4) == 0)
            return MCC_TABLE[i].risk;
    }
    return 0.5f;
}

// Fast integer parsing for hour/dow from ISO8601 timestamp
// Format: "2025-01-15T14:30:00Z" or with timezone offset
static void parse_datetime(const char *s, size_t len, int *hour, int *dow) {
    *hour = 0; *dow = 0;
    if (!s || len < 19) return;
    *hour = (s[11] - '0') * 10 + (s[12] - '0');

    // Compute day of week using Tomohiko Sakamoto's algorithm
    int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    int m = (s[5]-'0')*10 + (s[6]-'0');
    int d = (s[8]-'0')*10 + (s[9]-'0');
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    int w = (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
    // w: 0=Sun,1=Mon,...,6=Sat -> we need 0=Mon,...,6=Sun
    *dow = (w + 6) % 7;
}

// Parse timestamp to unix-like seconds (simplified, UTC)
static long parse_timestamp_seconds(const char *s, size_t len) {
    if (!s || len < 19) return 0;
    int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    int m = (s[5]-'0')*10 + (s[6]-'0');
    int d = (s[8]-'0')*10 + (s[9]-'0');
    int h = (s[11]-'0')*10 + (s[12]-'0');
    int mi = (s[14]-'0')*10 + (s[15]-'0');
    int sc = (s[17]-'0')*10 + (s[18]-'0');
    // Days from year 0 (simplified)
    long days = (long)(y-1)*365 + (y-1)/4 - (y-1)/100 + (y-1)/400;
    static const int mdays[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    days += mdays[m-1] + d - 1;
    if (m > 2 && ((y%4==0 && y%100!=0) || y%400==0)) days++;
    return days * 86400 + h * 3600 + mi * 60 + sc;
}

// Full request processing: JSON parse + vectorize + search
// Returns fraud_count (0-5), or -1 on parse error
int ivf_process_request(const char *json_body, size_t json_len) {
    if (!g_idx.centroids) return -1;

    yyjson_doc *doc = yyjson_read(json_body, json_len, 0);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *tx   = yyjson_obj_get(root, "transaction");
    yyjson_val *cust = yyjson_obj_get(root, "customer");
    yyjson_val *merch = yyjson_obj_get(root, "merchant");
    yyjson_val *term = yyjson_obj_get(root, "terminal");
    yyjson_val *last = yyjson_obj_get(root, "last_transaction");

    if (!tx || !cust || !merch || !term) { yyjson_doc_free(doc); return -1; }

    float amount = (float)get_num(yyjson_obj_get(tx, "amount"));
    int installments = (int)get_num(yyjson_obj_get(tx, "installments"));
    yyjson_val *req_at_val = yyjson_obj_get(tx, "requested_at");
    const char *req_at = yyjson_get_str(req_at_val);
    size_t req_at_len = yyjson_get_len(req_at_val);

    float cust_avg = (float)get_num(yyjson_obj_get(cust, "avg_amount"));
    int tx_count_24h = (int)get_num(yyjson_obj_get(cust, "tx_count_24h"));

    yyjson_val *merch_id_val = yyjson_obj_get(merch, "id");
    const char *merch_id = yyjson_get_str(merch_id_val);
    size_t merch_id_len = yyjson_get_len(merch_id_val);
    yyjson_val *mcc_val = yyjson_obj_get(merch, "mcc");
    const char *mcc = yyjson_get_str(mcc_val);
    size_t mcc_len = yyjson_get_len(mcc_val);
    float merch_avg = (float)get_num(yyjson_obj_get(merch, "avg_amount"));

    float km_home = (float)get_num(yyjson_obj_get(term, "km_from_home"));
    int is_online = yyjson_get_bool(yyjson_obj_get(term, "is_online")) ? 1 : 0;
    int card_present = yyjson_get_bool(yyjson_obj_get(term, "card_present")) ? 1 : 0;

    // Check unknown merchant
    int unknown_merchant = 1;
    yyjson_val *known = yyjson_obj_get(cust, "known_merchants");
    if (known) {
        yyjson_val *km_val;
        yyjson_arr_iter iter;
        yyjson_arr_iter_init(known, &iter);
        while ((km_val = yyjson_arr_iter_next(&iter))) {
            const char *km_str = yyjson_get_str(km_val);
            size_t km_len = yyjson_get_len(km_val);
            if (km_len == merch_id_len && memcmp(km_str, merch_id, km_len) == 0) {
                unknown_merchant = 0;
                break;
            }
        }
    }

    int hour, dow;
    parse_datetime(req_at, req_at_len, &hour, &dow);
    float mcc_risk = mcc_risk_lookup(mcc, mcc_len);

    int has_last_tx = 0;
    float minutes_since_last = 0.0f, km_from_last = 0.0f;
    if (last && !yyjson_is_null(last)) {
        has_last_tx = 1;
        yyjson_val *last_ts_val = yyjson_obj_get(last, "timestamp");
        const char *last_ts = yyjson_get_str(last_ts_val);
        size_t last_ts_len = yyjson_get_len(last_ts_val);
        long ts1 = parse_timestamp_seconds(req_at, req_at_len);
        long ts2 = parse_timestamp_seconds(last_ts, last_ts_len);
        minutes_since_last = (float)(ts1 - ts2) / 60.0f;
        km_from_last = (float)get_num(yyjson_obj_get(last, "km_from_current"));
    }

    yyjson_doc_free(doc);

    return ivf_fraud_score(amount, installments, cust_avg, tx_count_24h,
                           merch_avg, mcc_risk, km_home, is_online, card_present,
                           unknown_merchant, hour, dow,
                           has_last_tx, minutes_since_last, km_from_last);
}

void ivf_destroy(void) {
    for (int i = 0; i < g_idx.n_mmaps; i++) {
        if (g_idx.mmap_ptrs[i]) munmap(g_idx.mmap_ptrs[i], g_idx.mmap_sizes[i]);
    }
    memset(&g_idx, 0, sizeof(g_idx));
}
