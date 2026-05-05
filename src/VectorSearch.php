<?php

class VectorSearch
{
    private static ?\FFI $ffi = null;
    private static bool $initialized = false;

    public static function init(string $indexDir, string $libPath = null, int $nprobe = 10): void
    {
        if (self::$initialized) return;

        $libPath = $libPath ?? __DIR__ . '/libvector.so';

        self::$ffi = \FFI::cdef("
            int ivf_init(const char *index_dir, int nprobe);
            int ivf_search(const float *query, int *out_labels, float *out_distances, int k);
            void ivf_destroy(void);
            int ivf_fraud_score(
                float amount, int installments, float cust_avg,
                int tx_count_24h, float merch_avg, float mcc_risk,
                float km_home, int is_online, int card_present,
                int unknown_merchant, int hour, int dow,
                int has_last_tx, float minutes_since_last, float km_from_last);
            int ivf_process_request(const char *json_body, size_t json_len);
        ", $libPath);

        $result = self::$ffi->ivf_init($indexDir, $nprobe);
        if ($result !== 0) {
            throw new \RuntimeException("Failed to initialize IVF index from $indexDir");
        }

        self::$initialized = true;
    }

    /**
     * Combined vectorize+search+count in a single FFI call.
     * Returns fraud_count (0-5).
     */
    public static function scoreDirect(
        float $amount, int $installments, float $custAvg,
        int $txCount24h, float $merchAvg, float $mccRisk,
        float $kmHome, int $isOnline, int $cardPresent,
        int $unknownMerchant, int $hour, int $dow,
        int $hasLastTx, float $minutesSinceLast, float $kmFromLast
    ): int {
        return self::$ffi->ivf_fraud_score(
            $amount, $installments, $custAvg,
            $txCount24h, $merchAvg, $mccRisk,
            $kmHome, $isOnline, $cardPresent,
            $unknownMerchant, $hour, $dow,
            $hasLastTx, $minutesSinceLast, $kmFromLast
        );
    }

    /**
     * Full pipeline in C: JSON parse + vectorize + search + count.
     * Returns fraud_count (0-5), or -1 on parse error.
     */
    public static function processRequest(string $jsonBody): int
    {
        return self::$ffi->ivf_process_request($jsonBody, strlen($jsonBody));
    }

    public static function destroy(): void
    {
        if (self::$ffi && self::$initialized) {
            self::$ffi->ivf_destroy();
            self::$initialized = false;
        }
    }
}
