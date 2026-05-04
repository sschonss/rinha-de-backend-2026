<?php
require_once __DIR__ . '/../src/FraudDetector.php';

function assert_vector(string $name, array $input, array $expected): void {
    $actual = FraudDetector::vectorize($input);

    $pass = true;
    for ($i = 0; $i < 14; $i++) {
        if (abs($actual[$i] - $expected[$i]) > 0.001) {
            echo "FAIL [$name] dim $i: expected {$expected[$i]}, got {$actual[$i]}\n";
            $pass = false;
        }
    }
    if ($pass) {
        echo "PASS [$name]\n";
    }
}

// Test 1: Legit transaction (from spec REGRAS_DE_DETECCAO.md)
assert_vector('legit_spec_example', [
    'id' => 'tx-1329056812',
    'transaction' => ['amount' => 41.12, 'installments' => 2, 'requested_at' => '2026-03-11T18:45:53Z'],
    'customer' => ['avg_amount' => 82.24, 'tx_count_24h' => 3, 'known_merchants' => ['MERC-003', 'MERC-016']],
    'merchant' => ['id' => 'MERC-016', 'mcc' => '5411', 'avg_amount' => 60.25],
    'terminal' => ['is_online' => false, 'card_present' => true, 'km_from_home' => 29.23],
    'last_transaction' => null,
], [0.0041, 0.1667, 0.05, 0.7826, 0.3333, -1, -1, 0.0292, 0.15, 0, 1, 0, 0.15, 0.006]);

// Test 2: Fraud transaction (from spec REGRAS_DE_DETECCAO.md)
assert_vector('fraud_spec_example', [
    'id' => 'tx-3330991687',
    'transaction' => ['amount' => 9505.97, 'installments' => 10, 'requested_at' => '2026-03-14T05:15:12Z'],
    'customer' => ['avg_amount' => 81.28, 'tx_count_24h' => 20, 'known_merchants' => ['MERC-008', 'MERC-007', 'MERC-005']],
    'merchant' => ['id' => 'MERC-068', 'mcc' => '7802', 'avg_amount' => 54.86],
    'terminal' => ['is_online' => false, 'card_present' => true, 'km_from_home' => 952.27],
    'last_transaction' => null,
], [0.9506, 0.8333, 1.0, 0.2174, 0.8333, -1, -1, 0.9523, 1.0, 0, 1, 1, 0.75, 0.0055]);

// Test 3: Transaction with last_transaction (from API.md example)
assert_vector('with_last_tx', [
    'id' => 'tx-3576980410',
    'transaction' => ['amount' => 384.88, 'installments' => 3, 'requested_at' => '2026-03-11T20:23:35Z'],
    'customer' => ['avg_amount' => 769.76, 'tx_count_24h' => 3, 'known_merchants' => ['MERC-009', 'MERC-009', 'MERC-001', 'MERC-001']],
    'merchant' => ['id' => 'MERC-001', 'mcc' => '5912', 'avg_amount' => 298.95],
    'terminal' => ['is_online' => false, 'card_present' => true, 'km_from_home' => 13.7090520965],
    'last_transaction' => ['timestamp' => '2026-03-11T14:58:35Z', 'km_from_current' => 18.8626479774],
], [
    0.0385,   // 384.88 / 10000
    0.25,     // 3 / 12
    0.05,     // (384.88 / 769.76) / 10 = 0.5 / 10
    0.8696,   // 20 / 23
    0.3333,   // Wed=2, 2/6
    0.2257,   // minutes: (20:23:35 - 14:58:35) = 325 min, 325/1440
    0.0189,   // 18.8626 / 1000
    0.0137,   // 13.709 / 1000
    0.15,     // 3 / 20
    0,        // is_online=false
    1,        // card_present=true
    0,        // MERC-001 in known_merchants → known → 0
    0.20,     // mcc_risk[5912]
    0.0299,   // 298.95 / 10000
]);

// Test 4: Edge case — avg_amount = 0 (division by zero protection)
assert_vector('division_by_zero', [
    'id' => 'tx-edge',
    'transaction' => ['amount' => 100.0, 'installments' => 1, 'requested_at' => '2026-01-05T12:00:00Z'],
    'customer' => ['avg_amount' => 0, 'tx_count_24h' => 0, 'known_merchants' => []],
    'merchant' => ['id' => 'MERC-999', 'mcc' => '9999', 'avg_amount' => 0],
    'terminal' => ['is_online' => true, 'card_present' => false, 'km_from_home' => 0],
    'last_transaction' => null,
], [
    0.01,     // 100 / 10000
    0.0833,   // 1 / 12
    1.0,      // clamp(100/0 → INF → 1.0)
    0.5217,   // 12 / 23
    0.0,      // Mon=0, 0/6
    -1, -1,
    0.0,      // 0 / 1000
    0.0,      // 0 / 20
    1,        // is_online=true
    0,        // card_present=false
    1,        // MERC-999 not in [] → unknown → 1
    0.5,      // mcc 9999 not in map → default 0.5
    0.0,      // 0 / 10000
]);

echo "\nAll vectorization tests done.\n";
