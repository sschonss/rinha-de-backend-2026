<?php

class FraudDetector
{
    const RESPONSES = [
        0 => '{"approved":true,"fraud_score":0}',
        1 => '{"approved":true,"fraud_score":0.2}',
        2 => '{"approved":true,"fraud_score":0.4}',
        3 => '{"approved":false,"fraud_score":0.6}',
        4 => '{"approved":false,"fraud_score":0.8}',
        5 => '{"approved":false,"fraud_score":1}',
    ];

    const MCC_RISK = [
        '5411' => 0.15, '5812' => 0.30, '5912' => 0.20,
        '5944' => 0.45, '7801' => 0.80, '7802' => 0.75,
        '7995' => 0.85, '4511' => 0.35, '5311' => 0.25,
        '5999' => 0.50,
    ];

    /**
     * Returns pre-built JSON response string directly (avoids array alloc + json_encode)
     */
    public static function scoreToJson(array $data): string
    {
        $vector = self::vectorize($data);
        $labels = VectorSearch::query($vector);
        $fraudCount = $labels[0] + $labels[1] + $labels[2] + $labels[3] + $labels[4];
        return self::RESPONSES[$fraudCount];
    }

    public static function score(array $data): array
    {
        $vector = self::vectorize($data);
        $labels = VectorSearch::query($vector);
        $fraudCount = $labels[0] + $labels[1] + $labels[2] + $labels[3] + $labels[4];
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
        $hour = (int)gmdate('G', $ts);
        $dow = ((int)gmdate('N', $ts)) - 1;

        $avgAmount = $cust['avg_amount'];
        $amountVsAvg = ($avgAmount > 0)
            ? $tx['amount'] / ($avgAmount * 10.0)
            : 1.0;

        if ($last !== null) {
            $lastTs = strtotime($last['timestamp']);
            $minutes = ($ts - $lastTs) / 60.0;
            $v = $minutes / 1440.0;
            $minutesSinceLast = $v < 0.0 ? 0.0 : ($v > 1.0 ? 1.0 : $v);
            $v = $last['km_from_current'] / 1000.0;
            $kmFromLast = $v < 0.0 ? 0.0 : ($v > 1.0 ? 1.0 : $v);
        } else {
            $minutesSinceLast = -1.0;
            $kmFromLast = -1.0;
        }

        $v = $tx['amount'] / 10000.0;
        $d0 = $v < 0.0 ? 0.0 : ($v > 1.0 ? 1.0 : $v);
        $v = $tx['installments'] / 12.0;
        $d1 = $v < 0.0 ? 0.0 : ($v > 1.0 ? 1.0 : $v);
        $v = $amountVsAvg;
        $d2 = $v < 0.0 ? 0.0 : ($v > 1.0 ? 1.0 : $v);
        $v = $term['km_from_home'] / 1000.0;
        $d7 = $v < 0.0 ? 0.0 : ($v > 1.0 ? 1.0 : $v);
        $v = $cust['tx_count_24h'] / 20.0;
        $d8 = $v < 0.0 ? 0.0 : ($v > 1.0 ? 1.0 : $v);
        $v = $merch['avg_amount'] / 10000.0;
        $d13 = $v < 0.0 ? 0.0 : ($v > 1.0 ? 1.0 : $v);

        $unknown = 1.0;
        $mid = $merch['id'];
        foreach ($cust['known_merchants'] as $km) {
            if ($km === $mid) { $unknown = 0.0; break; }
        }

        return [
            $d0, $d1, $d2,
            $hour / 23.0,
            $dow / 6.0,
            $minutesSinceLast, $kmFromLast,
            $d7, $d8,
            $term['is_online'] ? 1.0 : 0.0,
            $term['card_present'] ? 1.0 : 0.0,
            $unknown,
            self::MCC_RISK[$merch['mcc']] ?? 0.5,
            $d13,
        ];
    }
}
