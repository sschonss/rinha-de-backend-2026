<?php

class VectorSearch
{
    private static ?\FFI $ffi = null;
    private static bool $initialized = false;

    public static function init(string $indexPath, ?string $libPath = null,
                                int $fastNprobe = 8, int $fullNprobe = 24): void
    {
        if (self::$initialized) return;

        $libPath ??= __DIR__ . '/libvector.so';

        self::$ffi = \FFI::cdef("
            int ivf_init(const char *index_path, int fast_nprobe, int mid_nprobe, int full_nprobe);
            void ivf_destroy(void);
            int ivf_warmup(int n_queries);
            int ivf_fraud_score(
                float amount, int installments, float cust_avg,
                int tx_count_24h, float merch_avg, float mcc_risk,
                float km_home, int is_online, int card_present,
                int unknown_merchant, int hour, int dow,
                int has_last_tx, float minutes_since_last, float km_from_last);
            int ivf_score_json(const char *json, int len);
        ", $libPath);

        $rc = self::$ffi->ivf_init($indexPath, $fastNprobe, 0, $fullNprobe);
        if ($rc !== 0) {
            throw new \RuntimeException("ivf_init failed for $indexPath");
        }
        self::$initialized = true;
    }

    public static function warmup(int $n): void
    {
        if (self::$ffi) {
            self::$ffi->ivf_warmup($n);
        }
    }

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

    public static function scoreJson(string $json): int
    {
        return self::$ffi->ivf_score_json($json, strlen($json));
    }

    public static function destroy(): void
    {
        if (self::$ffi && self::$initialized) {
            self::$ffi->ivf_destroy();
            self::$initialized = false;
        }
    }
}
