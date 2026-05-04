<?php
date_default_timezone_set('UTC');

class FraudDetector
{
    const MAX_AMOUNT = 10000;
    const MAX_INSTALLMENTS = 12;
    const AMOUNT_VS_AVG_RATIO = 10;
    const MAX_MINUTES = 1440;
    const MAX_KM = 1000;
    const MAX_TX_COUNT_24H = 20;
    const MAX_MERCHANT_AVG = 10000;

    const MCC_RISK = [
        '5411' => 0.15, '5812' => 0.30, '5912' => 0.20,
        '5944' => 0.45, '7801' => 0.80, '7802' => 0.75,
        '7995' => 0.85, '4511' => 0.35, '5311' => 0.25,
        '5999' => 0.50,
    ];

    public static function score(array $data): array
    {
        $vector = self::vectorize($data);
        $labels = VectorSearch::query($vector);
        $fraudCount = 0;
        for ($i = 0; $i < 5; $i++) {
            $fraudCount += $labels[$i];
        }
        $fraudScore = $fraudCount / 5.0;

        return [
            'approved' => $fraudScore < 0.6,
            'fraud_score' => $fraudScore,
        ];
    }

    public static function vectorize(array $d): array
    {
        $tx = $d['transaction'];
        $cust = $d['customer'];
        $merch = $d['merchant'];
        $term = $d['terminal'];
        $last = $d['last_transaction'];

        $ts = strtotime($tx['requested_at']);
        $hour = (int) gmdate('G', $ts);
        $dow = ((int) gmdate('N', $ts)) - 1; // Mon=0, Sun=6

        $avgAmount = $cust['avg_amount'];
        $amountVsAvg = ($avgAmount > 0)
            ? ($tx['amount'] / $avgAmount) / self::AMOUNT_VS_AVG_RATIO
            : 1.0;

        if ($last !== null) {
            $lastTs = strtotime($last['timestamp']);
            $minutes = ($ts - $lastTs) / 60.0;
            $minutesSinceLast = self::clamp($minutes / self::MAX_MINUTES);
            $kmFromLast = self::clamp($last['km_from_current'] / self::MAX_KM);
        } else {
            $minutesSinceLast = -1.0;
            $kmFromLast = -1.0;
        }

        return [
            self::clamp($tx['amount'] / self::MAX_AMOUNT),
            self::clamp($tx['installments'] / self::MAX_INSTALLMENTS),
            self::clamp($amountVsAvg),
            $hour / 23.0,
            $dow / 6.0,
            $minutesSinceLast,
            $kmFromLast,
            self::clamp($term['km_from_home'] / self::MAX_KM),
            self::clamp($cust['tx_count_24h'] / self::MAX_TX_COUNT_24H),
            $term['is_online'] ? 1.0 : 0.0,
            $term['card_present'] ? 1.0 : 0.0,
            isset(array_flip($cust['known_merchants'])[$merch['id']]) ? 0.0 : 1.0,
            self::MCC_RISK[$merch['mcc']] ?? 0.5,
            self::clamp($merch['avg_amount'] / self::MAX_MERCHANT_AVG),
        ];
    }

    private static function clamp(float $v): float
    {
        return max(0.0, min(1.0, $v));
    }
}
