#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

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

    float vectors[N_VECTORS * DIMS];
    uint8_t labels[N_VECTORS];
    float centroids[N_CLUSTERS * DIMS];
    uint32_t offsets[N_CLUSTERS * 2];

    srand(42);
    for (int c = 0; c < N_CLUSTERS; c++) {
        for (int d = 0; d < DIMS; d++) {
            centroids[c * DIMS + d] = c * 0.2f;
        }
        offsets[c * 2] = c * 20;
        offsets[c * 2 + 1] = 20;

        for (int i = 0; i < 20; i++) {
            int idx = c * 20 + i;
            for (int d = 0; d < DIMS; d++) {
                vectors[idx * DIMS + d] = c * 0.2f + (rand() % 100 - 50) * 0.001f;
            }
            labels[idx] = (c == 0) ? 1 : 0;
        }
    }

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

    extern int ivf_init(const char *index_dir, int nprobe);
    extern int ivf_search(const float *query, int *out_labels, float *out_distances, int k);
    extern void ivf_destroy(void);

    int ret = ivf_init(dir, 3);
    if (ret != 0) { printf("FAIL: ivf_init returned %d\n", ret); return 1; }
    printf("PASS: ivf_init\n");

    float query_fraud[DIMS];
    for (int d = 0; d < DIMS; d++) query_fraud[d] = 0.01f;

    int result_labels[5];
    float result_dists[5];
    ret = ivf_search(query_fraud, result_labels, result_dists, 5);
    if (ret != 0) { printf("FAIL: ivf_search returned %d\n", ret); return 1; }

    int fraud_count = 0;
    for (int i = 0; i < 5; i++) fraud_count += result_labels[i];
    printf("Query near cluster 0 (fraud): %d/5 fraud labels\n", fraud_count);
    if (fraud_count >= 4) printf("PASS: fraud detection\n");
    else printf("FAIL: expected mostly fraud, got %d/5\n", fraud_count);

    float query_legit[DIMS];
    for (int d = 0; d < DIMS; d++) query_legit[d] = 0.6f;

    ret = ivf_search(query_legit, result_labels, result_dists, 5);
    if (ret != 0) { printf("FAIL: ivf_search returned %d\n", ret); return 1; }

    int legit_count = 0;
    for (int i = 0; i < 5; i++) legit_count += (result_labels[i] == 0);
    printf("Query near cluster 3 (legit): %d/5 legit labels\n", legit_count);
    if (legit_count >= 4) printf("PASS: legit detection\n");
    else printf("FAIL: expected mostly legit, got %d/5\n", legit_count);

    int sorted = 1;
    for (int i = 0; i < 5; i++) {
        if (result_dists[i] < 0) { sorted = 0; break; }
    }
    printf("%s: distances non-negative\n", sorted ? "PASS" : "FAIL");

    ivf_destroy();
    printf("PASS: ivf_destroy\n");

    snprintf(path, sizeof(path), "rm -rf %s", dir);
    system(path);

    printf("\nAll C tests done.\n");
    return 0;
}
