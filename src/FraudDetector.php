<?php

class FraudDetector
{
    public const RESPONSES = [
        0 => '{"approved":true,"fraud_score":0.0}',
        1 => '{"approved":true,"fraud_score":0.2}',
        2 => '{"approved":true,"fraud_score":0.4}',
        3 => '{"approved":false,"fraud_score":0.6}',
        4 => '{"approved":false,"fraud_score":0.8}',
        5 => '{"approved":false,"fraud_score":1.0}',
    ];

    private const MCC_RISK = [
        '5411' => 0.15, '5812' => 0.30, '5912' => 0.20,
        '5944' => 0.45, '7801' => 0.80, '7802' => 0.75,
        '7995' => 0.85, '4511' => 0.35, '5311' => 0.25,
        '5999' => 0.50,
    ];

    public static function scoreToJson(array $data): string
    {
        $tx    = $data['transaction'];
        $cust  = $data['customer'];
        $merch = $data['merchant'];
        $term  = $data['terminal'];
        $last  = $data['last_transaction'] ?? null;

        $ts   = strtotime($tx['requested_at']);
        $hour = (int)gmdate('G', $ts);
        $dow  = ((int)gmdate('N', $ts)) - 1;

        $mccRisk = self::MCC_RISK[$merch['mcc']] ?? 0.5;

        $unknown = 1;
        $mid = $merch['id'];
        foreach ($cust['known_merchants'] as $km) {
            if ($km === $mid) { $unknown = 0; break; }
        }

        if ($last !== null) {
            $lastTs  = strtotime($last['timestamp']);
            $minutes = ($ts - $lastTs) / 60.0;
            $count = VectorSearch::scoreDirect(
                (float)$tx['amount'], (int)$tx['installments'], (float)$cust['avg_amount'],
                (int)$cust['tx_count_24h'], (float)$merch['avg_amount'], $mccRisk,
                (float)$term['km_from_home'], $term['is_online'] ? 1 : 0, $term['card_present'] ? 1 : 0,
                $unknown, $hour, $dow,
                1, (float)$minutes, (float)$last['km_from_current']
            );
        } else {
            $count = VectorSearch::scoreDirect(
                (float)$tx['amount'], (int)$tx['installments'], (float)$cust['avg_amount'],
                (int)$cust['tx_count_24h'], (float)$merch['avg_amount'], $mccRisk,
                (float)$term['km_from_home'], $term['is_online'] ? 1 : 0, $term['card_present'] ? 1 : 0,
                $unknown, $hour, $dow,
                0, 0.0, 0.0
            );
        }
        return self::RESPONSES[$count] ?? self::RESPONSES[0];
    }
}
