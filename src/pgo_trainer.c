// PGO training driver: exercises ivf_score_json with representative payloads.
// Compiled with -fprofile-generate, run once, .gcda files used by -fprofile-use rebuild.
#include <stdio.h>
#include <string.h>
#include "vector_search.h"

static const char *PAYLOADS[] = {
    "{\"id\":\"tx-1\",\"transaction\":{\"amount\":41.12,\"installments\":2,\"requested_at\":\"2026-03-11T18:45:53Z\"},\"customer\":{\"avg_amount\":82.24,\"tx_count_24h\":3,\"known_merchants\":[\"MERC-003\",\"MERC-016\"]},\"merchant\":{\"id\":\"MERC-016\",\"mcc\":\"5411\",\"avg_amount\":60.25},\"terminal\":{\"is_online\":false,\"card_present\":true,\"km_from_home\":2.5},\"last_transaction\":null}",
    "{\"id\":\"tx-2\",\"transaction\":{\"amount\":9505.97,\"installments\":10,\"requested_at\":\"2026-03-14T05:15:12Z\"},\"customer\":{\"avg_amount\":40.0,\"tx_count_24h\":3,\"known_merchants\":[\"MERC-1\"]},\"merchant\":{\"id\":\"UNKNOWN\",\"mcc\":\"7995\",\"avg_amount\":4500.0},\"terminal\":{\"is_online\":true,\"card_present\":false,\"km_from_home\":850.0},\"last_transaction\":{\"timestamp\":\"2026-03-14T04:30:00Z\",\"km_from_current\":900.0}}",
    "{\"id\":\"tx-3\",\"transaction\":{\"amount\":150.00,\"installments\":1,\"requested_at\":\"2026-04-02T09:12:04Z\"},\"customer\":{\"avg_amount\":120.0,\"tx_count_24h\":12,\"known_merchants\":[\"MERC-100\",\"MERC-200\",\"MERC-300\"]},\"merchant\":{\"id\":\"MERC-100\",\"mcc\":\"5812\",\"avg_amount\":135.0},\"terminal\":{\"is_online\":true,\"card_present\":true,\"km_from_home\":0.8},\"last_transaction\":{\"timestamp\":\"2026-04-02T09:00:00Z\",\"km_from_current\":1.0}}",
    "{\"id\":\"tx-4\",\"transaction\":{\"amount\":2500.00,\"installments\":6,\"requested_at\":\"2026-02-20T23:55:00Z\"},\"customer\":{\"avg_amount\":80.0,\"tx_count_24h\":1,\"known_merchants\":[]},\"merchant\":{\"id\":\"MERC-999\",\"mcc\":\"6011\",\"avg_amount\":2800.0},\"terminal\":{\"is_online\":true,\"card_present\":false,\"km_from_home\":1500.0},\"last_transaction\":null}",
    "{\"id\":\"tx-5\",\"transaction\":{\"amount\":12.50,\"installments\":1,\"requested_at\":\"2026-01-15T12:30:45Z\"},\"customer\":{\"avg_amount\":15.0,\"tx_count_24h\":8,\"known_merchants\":[\"MERC-A\",\"MERC-B\"]},\"merchant\":{\"id\":\"MERC-A\",\"mcc\":\"5411\",\"avg_amount\":13.0},\"terminal\":{\"is_online\":true,\"card_present\":true,\"km_from_home\":3.2},\"last_transaction\":{\"timestamp\":\"2026-01-15T11:45:00Z\",\"km_from_current\":2.0}}",
    "{\"id\":\"tx-6\",\"transaction\":{\"amount\":750.00,\"installments\":3,\"requested_at\":\"2026-04-22T16:10:00Z\"},\"customer\":{\"avg_amount\":300.0,\"tx_count_24h\":5,\"known_merchants\":[\"MERC-X\"]},\"merchant\":{\"id\":\"MERC-X\",\"mcc\":\"5732\",\"avg_amount\":700.0},\"terminal\":{\"is_online\":false,\"card_present\":true,\"km_from_home\":12.5},\"last_transaction\":{\"timestamp\":\"2026-04-22T14:00:00Z\",\"km_from_current\":15.0}}",
    "{\"id\":\"tx-7\",\"transaction\":{\"amount\":5000.00,\"installments\":12,\"requested_at\":\"2026-03-30T03:22:11Z\"},\"customer\":{\"avg_amount\":250.0,\"tx_count_24h\":2,\"known_merchants\":[\"MERC-Y\",\"MERC-Z\"]},\"merchant\":{\"id\":\"MERC-NEW\",\"mcc\":\"4829\",\"avg_amount\":4800.0},\"terminal\":{\"is_online\":true,\"card_present\":false,\"km_from_home\":2200.0},\"last_transaction\":{\"timestamp\":\"2026-03-30T02:00:00Z\",\"km_from_current\":2500.0}}",
    "{\"id\":\"tx-8\",\"transaction\":{\"amount\":89.90,\"installments\":1,\"requested_at\":\"2026-05-01T19:00:00Z\"},\"customer\":{\"avg_amount\":95.0,\"tx_count_24h\":4,\"known_merchants\":[\"MERC-FOOD\"]},\"merchant\":{\"id\":\"MERC-FOOD\",\"mcc\":\"5812\",\"avg_amount\":85.0},\"terminal\":{\"is_online\":true,\"card_present\":true,\"km_from_home\":5.0},\"last_transaction\":null}",
};
#define N_PAYLOADS (sizeof(PAYLOADS)/sizeof(PAYLOADS[0]))

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/data/index.bin";
    if (ivf_init(path, 8, 24) != 0) {
        fprintf(stderr, "ivf_init failed\n");
        return 1;
    }
    ivf_warmup(2000);

    // ~50k iterations across mixed payloads ≈ realistic K6 distribution
    long total = 0;
    for (int iter = 0; iter < 6000; iter++) {
        for (size_t i = 0; i < N_PAYLOADS; i++) {
            total += ivf_score_json(PAYLOADS[i], (int)strlen(PAYLOADS[i]));
        }
    }
    fprintf(stderr, "trainer: total=%ld\n", total);
    ivf_destroy();
    return 0;
}
