# 01 — Análise da Competição

## O que é a Rinha de Backend 2026?

A **Rinha de Backend** é uma competição brasileira onde desenvolvedores criam APIs de alta performance com recursos extremamente limitados. Em 2026, o tema é **detecção de fraudes em tempo real** usando busca por similaridade vetorial.

**Repositório oficial:** https://github.com/zanfranceschi/rinha-de-backend-2026

---

## Regras Fundamentais

### Endpoint
```
POST /fraud-score
```

Recebe uma transação financeira e retorna se é fraudulenta ou não, baseado na similaridade com 3 milhões de vetores de referência.

### Request Body
```json
{
  "id": "tx-1329056812",
  "transaction": {
    "amount": 2508.13,
    "installments": 7,
    "requested_at": "2026-03-11T03:45:53Z"
  },
  "customer": {
    "avg_amount": 209.74,
    "tx_count_24h": 13,
    "known_merchants": ["MERC-003", "MERC-016"]
  },
  "merchant": {
    "id": "MERC-089",
    "mcc": "5912",
    "avg_amount": 25.15
  },
  "terminal": {
    "is_online": false,
    "card_present": true,
    "km_from_home": 667.73
  },
  "last_transaction": {
    "timestamp": "2026-03-11T14:58:35Z",
    "km_from_current": 18.86
  }
}
```

### Response
```json
{
  "approved": false,
  "fraud_score": 1.0
}
```

### Vetorização
A transação é convertida em um vetor de **14 dimensões**:
1. `amount / 10000` (normalizado)
2. `installments / 12`
3. `amount / (customer_avg * 10)` (desvio do padrão)
4. `hour / 23` (hora do dia)
5. `day_of_week / 6` (dia da semana, 0=seg)
6. `minutes_since_last_tx / 1440` (ou -1 se não tem)
7. `km_from_last_tx / 1000` (ou -1 se não tem)
8. `km_from_home / 1000`
9. `tx_count_24h / 20`
10. `is_online` (0 ou 1)
11. `card_present` (0 ou 1)
12. `unknown_merchant` (0 ou 1)
13. `mcc_risk` (tabela de lookup)
14. `merchant_avg / 10000`

### Busca KNN
Após vetorizar, busca os **5 vizinhos mais próximos (K=5)** nos 3 milhões de vetores de referência. Cada vizinho tem um label (0=legítimo, 1=fraude). O `fraud_score` é a proporção de vizinhos fraudulentos:
- 0 vizinhos fraude → score 0.0 (approved: true)
- 1 vizinho → 0.2 (approved: true)
- 2 vizinhos → 0.4 (approved: true)
- 3 vizinhos → 0.6 (approved: false)
- 4 vizinhos → 0.8 (approved: false)
- 5 vizinhos → 1.0 (approved: false)

---

## Sistema de Scoring

O score final vai de **-6000 a +6000** e é composto por dois componentes:

### Score de Latência (p99)
```
score_p99 = 1000 × log10(1000 / max(p99_ms, 1))
```
- Cap: ±3000
- Cut: p99 > 2000ms → score = -3000

| p99 (ms) | Score |
|-----------|-------|
| 1 | +3000 (máximo) |
| 10 | +2000 |
| 30 | +1526 |
| 100 | +1000 |
| 1000 | 0 |
| 2000+ | -3000 (cut) |

**Insight crucial:** cada redução de 10x na latência = +1000 pontos.

### Score de Detecção
Baseado em erros ponderados:
- **False Positive (FP):** peso 1
- **False Negative (FN):** peso 3
- **HTTP Error:** peso 5
- Cut: failure_rate > 15% → score = -3000

### Fórmula Final
```
final_score = score_p99 + score_detecção
```

---

## Limites de Recursos

| Recurso | Limite |
|---------|--------|
| CPU total | 1 core |
| RAM total | 350 MB |
| Instâncias da API | Exatamente 2 |
| Load balancer | Obrigatório |
| Rede | Docker network interna |

### Distribuição que escolhemos:
- HAProxy: 0.05 CPU, 25 MB
- API-1: 0.475 CPU, 162 MB
- API-2: 0.475 CPU, 162 MB
- **Total:** 1.0 CPU, 349 MB

---

## Dataset

- **3 milhões de vetores de referência** (14 dims × float32 = 56 bytes cada)
- Total: ~160 MB de dados vetoriais
- Disponível como download (600 MB zip → descomprime para ~1.2 GB com labels e metadata)
- **54.100 transações de teste** com labels conhecidos
- Mix: 44.5% fraude, 55.5% legítimo
- 797 edge cases (1.47%)

---

## Teste de Carga (k6)

```
Executor: ramping-arrival-rate
Duração: 120 segundos
Taxa final: 900 req/s
VUs: até 250
Total: ~54.100 requests
```

O teste rampeia linearmente de 1 req/s até 900 req/s em 2 minutos, enviando todas as 54.100 transações do dataset.

---

## Hardware de Teste

**Mac Mini Late 2014**
- Intel Core i5-4278U (Haswell, 2.6 GHz, 2 cores / 4 threads)
- Suporte a: SSE4.2, AVX2, FMA
- 8 GB DDR3L 1600 MHz
- SSD
- Ubuntu 24.04 LTS

**Implicação:** podemos usar instruções AVX2 para SIMD (8 floats por ciclo)!
