# 07 — Resultados Oficiais

## Histórico de Testes

| # | Issue | Versão | Score | p99 | Det | Errors | Status |
|---|-------|--------|-------|-----|-----|--------|--------|
| 1 | #1261 | v1 | — | — | — | — | ❌ Falhou (PR não mergeado) |
| 2 | #1263 | AVX2+combined | **+4526.21** | 29.77ms | 3000 | 0 | ✅ |
| 3 | #1342 | yyjson (bugado) | **-6000** | 2002ms | -3000 | 10271 | 💀 Bug |
| 4 | #1356 | yyjson (corrigido) | **???** | ??? | ??? | ??? | ⏳ Pendente |

---

## Melhor Resultado: Issue #1263

### Score Breakdown
```
════════════════════════════════════════════════
  📊 RESULTADO OFICIAL — Mac Mini (Haswell)
════════════════════════════════════════════════
  
  p99 latência:     29.77 ms
  score_p99:        +1526.21
  
  True Positives:   24058 / 24058 (100%)
  True Negatives:   30042 / 30042 (100%)
  False Positives:  0
  False Negatives:  0
  HTTP Errors:      0
  score_detecção:   +3000.00 (PERFEITO)
  
  ────────────────────────────────────────────
  ⭐ SCORE FINAL:    +4526.21 / 6000
  ────────────────────────────────────────────
  
  Failure Rate:     0.00%
  Total Requests:   54100
  
════════════════════════════════════════════════
```

### O que isso significa?

- **Detecção perfeita:** classificamos TODAS as 54.100 transações corretamente
- **Zero downtime:** nenhum request falhou, nenhum timeout
- **p99 de 29.77ms:** 99% das requests completaram em menos de 30ms
- **Score 4526/6000:** ~75% do máximo possível

### Posição Estimada

Na época do teste, isso colocaria a solução entre as **top 10-20** da competição (baseado no leaderboard informal).

---

## Análise de Latência

### Distribuição esperada (p99 = 29.77ms)
```
p50:  ~5-10ms   (mediana)
p90:  ~15-20ms  (90th percentile)
p95:  ~22-27ms  (95th percentile)
p99:  29.77ms   (99th percentile — score usa este)
max:  ~50-100ms (outliers)
```

### Onde vai o tempo?
```
┌─────────────────────────────────────────────┐
│  Breakdown estimado de uma request (30ms)   │
├─────────────────────────────────────────────┤
│                                             │
│  ┌─────┐ HAProxy routing        ~1ms       │
│  │░░░░░│                                   │
│  ├─────┤                                   │
│  │█████│ Swoole HTTP parse      ~5ms       │
│  │█████│                                   │
│  ├─────┤                                   │
│  │████ │ PHP json_decode        ~5ms       │
│  │████ │ + array access                    │
│  ├─────┤                                   │
│  │███  │ FFI call overhead      ~3ms       │
│  ├─────┤                                   │
│  │██   │ C: IVF search         ~0.012ms   │
│  ├─────┤                                   │
│  │████ │ PHP response build     ~3ms       │
│  ├─────┤                                   │
│  │█████│ Swoole HTTP response   ~5ms       │
│  ├─────┤                                   │
│  │███  │ Network (Docker)       ~3ms       │
│  └─────┘                                   │
│                                             │
│  Total:                        ~25-30ms    │
└─────────────────────────────────────────────┘
```

**Insight:** a computação em C é apenas 0.04% do tempo total! O gargalo é todo o stack PHP/Swoole/Network.

---

## Teste Falhado: Issue #1342 (yyjson bugado)

### Score Breakdown
```
════════════════════════════════════════════════
  📊 RESULTADO — yyjson com -ffast-math
════════════════════════════════════════════════
  
  p99 latência:     2002.16 ms (TIMEOUT!)
  score_p99:        -3000 (cut triggered)
  
  True Positives:   2484
  True Negatives:   3165
  False Positives:  0
  False Negatives:  1
  HTTP Errors:      10271
  score_detecção:   -3000 (cut: failure_rate > 15%)
  
  ────────────────────────────────────────────
  ⭐ SCORE FINAL:    -6000 (MÍNIMO POSSÍVEL)
  ────────────────────────────────────────────
  
  Failure Rate:     64.52%
  Requests OK:      5650 / 15921 observados
  
════════════════════════════════════════════════
```

### Análise do Crash Pattern
```
Total esperado:  54100 requests
Observados:      15921 responses (TP+TN+FP+FN+Err)
Não respondidos: 38179 (timeouts — nem contaram como erro)
```

O servidor provavelmente:
1. Iniciou normalmente (primeiros requests OK)
2. Começou a crashar em inputs específicos (segfault no yyjson)
3. Swoole reiniciou workers, mas ficou em loop de crash
4. Requests acumularam → timeouts massivos

---

## Evolução de Scores (Gráfico ASCII)

```
Score
+6000 ┤ ═══════════════════════════════════ MAX
      │
+5000 ┤                     ╭── meta (yyjson fix)
      │                    ╱
+4526 ┤ ─ ─ ─ ─ ─ ─ ─ ─ ●─ ─ ─ AVX2 nativo
      │                 ╱
+3000 ┤               ╱
      │             ╱
+2000 ┤           ╱
      │         ╱
+1322 ┤ ─ ─ ─●─ ─ ─ ─ ─ ─ ─ ─ AVX2 (QEMU)
      │     ╱
    0 ┤ ──╱──────────────────────────────────
      │ ╱
-3813 ┤● ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ baseline
      │
-6000 ┤ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ● yyjson BUG
      │
      └──┬──────┬──────┬──────┬──────── Tempo
       v0.1   v0.2   v0.3   v0.4
```

---

## Comparação com Competição

### Score Distribution (estimada, ~500 participantes)
```
Score Range    | Participantes | Nosso?
───────────────┼──────────────┼───────
+5500 a +6000 | ~5           |
+5000 a +5500 | ~10          |
+4500 a +5000 | ~20          | ← AQUI (4526)
+4000 a +4500 | ~30          |
+3000 a +4000 | ~50          |
+1000 a +3000 | ~80          |
    0 a +1000 | ~100         |
-3000 a    0  | ~100         |
-6000 a -3000 | ~105         |
```

### O que os top scorers provavelmente usam:
- Rust/C puro (sem interpreter overhead)
- Custom HTTP parser (sem framework overhead)
- io_uring para I/O assíncrono
- Pre-computed index no startup
- Thread-per-core architecture

### Nossa vantagem competitiva:
- **Detecção perfeita** — muitos competidores sacrificam precisão por velocidade
- **Stack incomum** — PHP + C FFI é raramente visto, mas efetivo
- **IVF bem tunado** — K=4000, nprobe=5 dá o equilíbrio ideal

---

## Próximos Passos

### Se score do fix #1356 for > 4526:
1. ✅ Missão cumprida — yyjson melhorou latência
2. Documentar resultado final
3. Considerar mais otimizações se gap para top é grande

### Otimizações futuras possíveis:
1. **Pre-allocate yyjson buffers** — evita malloc/free por request
2. **Connection keepalive tuning** no HAProxy
3. **Swoole coroutine tuning** — buffer sizes, etc
4. **Batch centroid distance** — SIMD em múltiplos centroids por vez
5. **Quantized index** — int8 ao invés de float32 (4x menos memória bandwidth)
6. **Custom HTTP parser** em C (eliminar Swoole do hot path)
