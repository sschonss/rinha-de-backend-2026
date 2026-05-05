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

    // Cheap rules thresholds — skip KNN for obvious cases
    const CHEAP_MIN = 4;   // score < 4 → clearly not fraud
    const CHEAP_MAX = 17;  // score > 17 → clearly fraud

    /**
     * Cheap rules: count risk signals without KNN.
     * Returns 0-21 score based on simple threshold checks.
     */
    private static function cheapScore(array $data): int
    {
        $tx = $data['transaction'];
        $cust = $data['customer'];
        $merch = $data['merchant'];
        $term = $data['terminal'];
        $last = $data['last_transaction'];
        $score = 0;

        $amount = $tx['amount'];
        if ($amount >= 2000) $score++;
        if ($amount >= 500)  $score++;

        $installments = $tx['installments'];
        if ($installments >= 6) $score++;
        if ($installments >= 4) $score++;

        $avgAmount = $cust['avg_amount'];
        if ($avgAmount > 0) {
            $ratio = $amount / $avgAmount;
            if ($ratio >= 8) $score++;
            if ($ratio >= 1) $score++;
        }

        $ts = strtotime($tx['requested_at']);
        $hour = (int)gmdate('G', $ts);
        if ($hour < 7) $score++;
        if ($hour < 8 || $hour >= 21) $score++;

        if ($last !== null) {
            $lastTs = strtotime($last['timestamp']);
            $mins = abs(($ts - $lastTs) / 60);
            if ($mins <= 10) $score++;
            if ($mins <= 30) $score++;

            $lastKm = $last['km_from_current'];
            if ($lastKm >= 200) $score++;
            if ($lastKm >= 20) $score++;
        }

        $kmFromHome = $term['km_from_home'];
        if ($kmFromHome >= 200) $score++;
        if ($kmFromHome >= 50) $score++;

        $txCount = $cust['tx_count_24h'];
        if ($txCount >= 8) $score++;
        if ($txCount >= 6) $score++;

        if ($term['is_online']) $score++;
        if (!$term['card_present']) $score++;

        $mid = $merch['id'];
        $known = false;
        foreach ($cust['known_merchants'] as $km) {
            if ($km === $mid) { $known = true; break; }
        }
        if (!$known) $score++;

        $mccRisk = self::MCC_RISK[$merch['mcc']] ?? 0.5;
        if ($mccRisk >= 0.75) $score++;
        if ($mccRisk >= 0.45) $score++;

        $merchantAvg = $merch['avg_amount'];
        if ($merchantAvg <= 100) $score++;

        return $score;
    }

    /**
     * Single FFI call: PHP extracts fields → C does vectorize+search+count.
     * With cheap rules: skips KNN for obvious fraud/non-fraud cases.
     */
    public static function scoreToJson(array $data): string
    {
        // Cheap rules — skip KNN for obvious cases
        $cheapScore = self::cheapScore($data);
        if ($cheapScore < self::CHEAP_MIN) {
            return self::RESPONSES[0]; // clearly not fraud
        }
        if ($cheapScore > self::CHEAP_MAX) {
            return self::RESPONSES[5]; // clearly fraud
        }

        // Ambiguous — do full KNN search
        $tx = $data['transaction'];
        $cust = $data['customer'];
        $merch = $data['merchant'];
        $term = $data['terminal'];
        $last = $data['last_transaction'];

        $ts = strtotime($tx['requested_at']);
        $hour = (int)gmdate('G', $ts);
        $dow = ((int)gmdate('N', $ts)) - 1;

        $mccRisk = self::MCC_RISK[$merch['mcc']] ?? 0.5;

        $unknown = 1;
        $mid = $merch['id'];
        foreach ($cust['known_merchants'] as $km) {
            if ($km === $mid) { $unknown = 0; break; }
        }

        if ($last !== null) {
            $lastTs = strtotime($last['timestamp']);
            $minutes = ($ts - $lastTs) / 60.0;
            $fraudCount = VectorSearch::scoreDirect(
                $tx['amount'], $tx['installments'], $cust['avg_amount'],
                $cust['tx_count_24h'], $merch['avg_amount'], $mccRisk,
                $term['km_from_home'], $term['is_online'] ? 1 : 0, $term['card_present'] ? 1 : 0,
                $unknown, $hour, $dow,
                1, (float)$minutes, (float)$last['km_from_current']
            );
        } else {
            $fraudCount = VectorSearch::scoreDirect(
                $tx['amount'], $tx['installments'], $cust['avg_amount'],
                $cust['tx_count_24h'], $merch['avg_amount'], $mccRisk,
                $term['km_from_home'], $term['is_online'] ? 1 : 0, $term['card_present'] ? 1 : 0,
                $unknown, $hour, $dow,
                0, 0.0, 0.0
            );
        }

        return self::RESPONSES[$fraudCount];
    }

    // Keep for unit tests
    public static function score(array $data): array
    {
        $vector = self::vectorize($data);
        return ['vector' => $vector];
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
