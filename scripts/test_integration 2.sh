#!/usr/bin/env bash
set -uo pipefail

BASE_URL="http://localhost:9999"
PASS=0
FAIL=0

check() {
    local name="$1" expected="$2" actual="$3"
    if [[ "$actual" == *"$expected"* ]]; then
        echo "PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $name — expected '$expected', got '$actual'"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== Integration Tests ==="
echo ""

# Test 1: /ready endpoint
echo "--- Testing /ready ---"
READY=$(curl -s -o /dev/null -w "%{http_code}" "$BASE_URL/ready")
check "/ready returns 200" "200" "$READY"

# Test 2: /fraud-score with legit transaction (from spec)
echo ""
echo "--- Testing /fraud-score (legit) ---"
RESPONSE=$(curl -s -X POST "$BASE_URL/fraud-score" \
    -H "Content-Type: application/json" \
    -d '{
        "id": "tx-1329056812",
        "transaction": {"amount": 41.12, "installments": 2, "requested_at": "2026-03-11T18:45:53Z"},
        "customer": {"avg_amount": 82.24, "tx_count_24h": 3, "known_merchants": ["MERC-003", "MERC-016"]},
        "merchant": {"id": "MERC-016", "mcc": "5411", "avg_amount": 60.25},
        "terminal": {"is_online": false, "card_present": true, "km_from_home": 29.23},
        "last_transaction": null
    }')
echo "Response: $RESPONSE"
check "has approved field" "approved" "$RESPONSE"
check "has fraud_score field" "fraud_score" "$RESPONSE"

# Test 3: /fraud-score with fraud transaction (from spec)
echo ""
echo "--- Testing /fraud-score (fraud) ---"
RESPONSE=$(curl -s -X POST "$BASE_URL/fraud-score" \
    -H "Content-Type: application/json" \
    -d '{
        "id": "tx-3330991687",
        "transaction": {"amount": 9505.97, "installments": 10, "requested_at": "2026-03-14T05:15:12Z"},
        "customer": {"avg_amount": 81.28, "tx_count_24h": 20, "known_merchants": ["MERC-008", "MERC-007", "MERC-005"]},
        "merchant": {"id": "MERC-068", "mcc": "7802", "avg_amount": 54.86},
        "terminal": {"is_online": false, "card_present": true, "km_from_home": 952.27},
        "last_transaction": null
    }')
echo "Response: $RESPONSE"
check "has approved field" "approved" "$RESPONSE"
check "has fraud_score field" "fraud_score" "$RESPONSE"

# Test 4: Invalid JSON (should not return 500)
echo ""
echo "--- Testing error handling ---"
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE_URL/fraud-score" \
    -H "Content-Type: application/json" \
    -d 'this is not json')
check "invalid JSON returns 200 (not 500)" "200" "$HTTP_CODE"

# Test 5: 404 for unknown route
echo ""
echo "--- Testing 404 ---"
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$BASE_URL/nonexistent")
check "unknown route returns 404" "404" "$HTTP_CODE"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
exit $FAIL
