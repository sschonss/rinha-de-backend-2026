#ifndef VECTOR_SEARCH_H
#define VECTOR_SEARCH_H

#include <stdint.h>

int  ivf_init(const char *index_path, int fast_nprobe, int mid_nprobe, int full_nprobe);
void ivf_destroy(void);
int  ivf_warmup(int n_queries);

int  ivf_fraud_score(
    float amount, int installments, float cust_avg,
    int tx_count_24h, float merch_avg, float mcc_risk,
    float km_home, int is_online, int card_present,
    int unknown_merchant, int hour, int dow,
    int has_last_tx, float minutes_since_last, float km_from_last);

// All-in-C scoring: parse JSON request body, compute features, run KNN.
// Returns fraud count (0..5).
int  ivf_score_json(const char *json, int len);

#endif
