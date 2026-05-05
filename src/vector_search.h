#ifndef VECTOR_SEARCH_H
#define VECTOR_SEARCH_H

#include <stdint.h>
#include <stddef.h>

#define IVF_DIMS 14
#define IVF_K 5

int ivf_init(const char *index_dir, int nprobe);
int ivf_search(const float *query, int *out_labels, float *out_distances, int k);
void ivf_destroy(void);

// Combined: vectorize raw fields + search + count fraud labels → return fraud_count (0-5)
int ivf_fraud_score(
    float amount, int installments, float cust_avg,
    int tx_count_24h, float merch_avg, float mcc_risk,
    float km_home, int is_online, int card_present,
    int unknown_merchant, int hour, int dow,
    int has_last_tx, float minutes_since_last, float km_from_last);

// Full pipeline: JSON parse + vectorize + search + count → fraud_count (0-5), -1 on error
int ivf_process_request(const char *json_body, size_t json_len);

#endif
