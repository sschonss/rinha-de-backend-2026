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
