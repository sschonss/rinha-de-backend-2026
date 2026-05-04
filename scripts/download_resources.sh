#!/usr/bin/env bash
set -euo pipefail

REPO_BASE="https://github.com/zanfranceschi/rinha-de-backend-2026/raw/main/resources"
DEST="resources"

mkdir -p "$DEST"

echo "Downloading references.json.gz (~16MB)..."
curl -L -o "$DEST/references.json.gz" "$REPO_BASE/references.json.gz"

echo "Downloading mcc_risk.json..."
curl -L -o "$DEST/mcc_risk.json" "$REPO_BASE/mcc_risk.json"

echo "Downloading normalization.json..."
curl -L -o "$DEST/normalization.json" "$REPO_BASE/normalization.json"

echo "Downloading example-references.json (for testing)..."
curl -L -o "$DEST/example-references.json" "$REPO_BASE/example-references.json"

echo "Done. Files in $DEST/"
ls -lh "$DEST/"
