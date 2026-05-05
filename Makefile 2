.PHONY: up down restart logs ready smoke test results stats clean

up:
	docker compose up --build -d

down:
	docker compose down

restart: down up

logs:
	docker compose logs -f

ready:
	@for i in $$(seq 1 30); do \
		if curl -s http://localhost:9999/ready | grep -q OK; then \
			echo "✅ API ready!"; \
			exit 0; \
		fi; \
		echo "Waiting... ($$i/30)"; \
		sleep 3; \
	done; \
	echo "❌ API not ready after 90s"; exit 1

smoke: ready
	K6_NO_USAGE_REPORT=true k6 run test/smoke.js

test: ready
	K6_NO_USAGE_REPORT=true k6 run test/test.js
	@echo ""
	@echo "════════════════════════════════════════"
	@echo "  📊 RESULTADO DA RINHA"
	@echo "════════════════════════════════════════"
	@cat test/results.json | jq -r '"  p99:            \(.p99)\n  score_p99:      \(.scoring.p99_score.value)\n  score_detecção: \(.scoring.detection_score.value)\n  ────────────────────────────────\n  ⭐ SCORE FINAL:  \(.scoring.final_score)\n  ────────────────────────────────\n  TP: \(.scoring.breakdown.true_positive_detections)  TN: \(.scoring.breakdown.true_negative_detections)  FP: \(.scoring.breakdown.false_positive_detections)  FN: \(.scoring.breakdown.false_negative_detections)  Err: \(.scoring.breakdown.http_errors)\n  failure_rate:    \(.scoring.failure_rate)"'
	@echo "════════════════════════════════════════"

results:
	@cat test/results.json | jq

stats:
	@docker stats --no-stream --format "table {{.Name}}\t{{.CPUPerc}}\t{{.MemUsage}}\t{{.MemPerc}}"

clean:
	docker compose down -v --rmi local
