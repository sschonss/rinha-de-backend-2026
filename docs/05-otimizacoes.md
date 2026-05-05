# 05 — Jornada de Otimização

## Visão Geral

```
Score: -3813 ──► +1322 ──► +4526 ──► -6000 ──► ????
       │           │          │          │         │
    baseline    SSE2→AVX2  nativo     yyjson    fix
    (QEMU)     (QEMU)     (Mac Mini)  (BUG!)   (pendente)
```

---

## Round 0: Baseline (Score: -3813)

### O Problema
Primeira versão rodando em QEMU (emulação x86 em ARM Mac):
```
p99:            2001.49ms  (TIMEOUT!)
score_p99:      -3000
score_detecção: -813.71
failure_rate:    8.3%
⭐ SCORE FINAL:  -3813.71
```

### Diagnóstico
- QEMU é **10-50x mais lento** que hardware nativo
- A busca IVF que levaria ~12μs no nativo levava ~1ms no QEMU
- Com 900 req/s, cada request tinha budget de apenas ~1.1ms
- Resultado: timeouts massivos

### Lição
> Não dá para confiar em benchmarks locais com QEMU. O teste real no Mac Mini é a única referência confiável.

---

## Round 1: AVX2 + Combined C Function (Score: +1322 local, +4526 nativo)

### Mudanças

#### 1. SSE2 → AVX2
```diff
- // SSE2: 4 floats por ciclo
- __m128 d = _mm_sub_ps(_mm_loadu_ps(a), _mm_loadu_ps(b));
+ // AVX2: 8 floats por ciclo  
+ __m256 d = _mm256_sub_ps(_mm256_loadu_ps(a), _mm256_loadu_ps(b));
```
**Impacto:** 2x throughput no cálculo de distância

#### 2. N_CLUSTERS: 1500 → 4000
```
Antes: 3M / 1500 clusters × nprobe=5 = ~10.000 vetores por busca
Depois: 3M / 4000 clusters × nprobe=5 = ~3.750 vetores por busca
```
**Impacto:** 62% menos distâncias para calcular

#### 3. Combined C Function (`ivf_fraud_score`)
```
Antes: PHP vetoriza → FFI busca → PHP conta labels (3 FFI calls + PHP overhead)
Depois: PHP chama ivf_fraud_score() (1 FFI call, tudo em C)
```
**Impacto:** elimina overhead de 2 FFI calls + alocação de arrays PHP

#### 4. Compiler Flags
```
-O3 -march=haswell -mavx2 -ffast-math -funroll-loops
```
- `-march=haswell`: habilita todas as instruções do Haswell
- `-ffast-math`: permite reordenação de operações float
- `-funroll-loops`: desenrola loops curtos

#### 5. MAP_POPULATE
```c
mmap(NULL, size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
```
Pre-carrega todas as páginas no startup, eliminando page faults durante requests.

#### 6. WORKERS=1
Elimina context switching com apenas 0.475 CPU por instância.

#### 7. QEMU Fix para K-means
```dockerfile
FROM --platform=$BUILDPLATFORM python:3.11-slim AS indexer
```
O indexer (K-means) roda nativamente no ARM, evitando QEMU crash com 4000 clusters.

### Resultado Local (QEMU)
```
p99:            1593ms (melhorou de 2001ms)
score_p99:      -2594
score_detecção: +3000 (PERFEITO!)
⭐ SCORE LOCAL:  +1322.89
```

### Resultado Oficial (Mac Mini Haswell nativo)
```
p99:            29.77ms  ← WOW! 67x mais rápido que QEMU!
score_p99:      +1526.21
score_detecção: +3000 (0 FP, 0 FN, 0 Errors)
⭐ SCORE OFICIAL: +4526.21
```

---

## Round 2: yyjson — Eliminar PHP Completamente (Score: -6000 💀)

### Motivação
Com p99=29.77ms e detecção perfeita, a única forma de melhorar era reduzir latência. Análise do hot path:

```
PHP json_decode()  → ~5-10μs (estimativa)
PHP array access   → ~2-5μs
FFI call overhead  → ~1-2μs
C computation      → ~12μs
PHP json response  → ~1-2μs
─────────────────────────────
Total             → ~25-30μs + Swoole overhead
```

Se movermos TUDO para C (incluindo JSON parsing), eliminamos todo overhead PHP:

```
[C] yyjson parse   → ~0.5μs
[C] vectorize      → ~0.2μs
[C] IVF search     → ~12μs
─────────────────────────────
Total             → ~13μs + FFI call (~1μs) + Swoole
```

**Potencial:** 2x redução no p99 → +500 pontos no score!

### Implementação

1. **Adicionou yyjson 0.10** — o parser JSON mais rápido em C
2. **`ivf_process_request()`** — pipeline completo em uma função C
3. **Parser de datetime customizado** — Sakamoto's algorithm em C
4. **`get_num()` helper** — lida com int e real no JSON
5. **Responses pré-computadas** — array de strings constantes no PHP

### O Desastre

```
p99:            2002.16ms (TIMEOUT!)
HTTP Errors:    10.271 (64.52% failure rate!)
score_p99:      -3000
score_detecção: -3000
⭐ SCORE:        -6000 (PIOR POSSÍVEL)
```

### Root Cause: `-ffast-math` no yyjson.c
Ver [06-bug-ffast-math.md](./06-bug-ffast-math.md) para o post-mortem completo.

---

## Round 3: Fix (Resultado Pendente 🤞)

### Correções
1. Compilação separada: yyjson.o (sem -ffast-math) + vector_search.o (com -ffast-math)
2. `get_num()` helper para lidar com int/real uniformemente
3. NULL safety em parse_datetime e parse_timestamp_seconds
4. JIT desabilitado (incompatível com Swoole)

### Teste Local
```
5000/5000 requests OK
0 HTTP errors
4998/5000 detecção correta (99.96%)
```

### Expectativa
- Detecção deve manter 0 FP, 0 FN (score_det = +3000)
- p99 deve ser significativamente menor que 29.77ms (yyjson elimina ~15μs de PHP)
- **Score esperado: >4526** (potencialmente 5000+)

---

## Análise do Scoring

### Como melhorar de 4526 para 6000?

```
Score atual:  1526 (p99) + 3000 (det) = 4526
Score máximo: 3000 (p99) + 3000 (det) = 6000
Gap:          1474 pontos no p99
```

Para p99 score:
```
score_p99 = 1000 × log10(1000 / p99)

p99=29.77ms → score=1526
p99=10ms    → score=2000  (+474)
p99=3ms     → score=2523  (+997)
p99=1ms     → score=3000  (+1474)
```

| Meta | p99 necessário | Melhoria necessária |
|------|---------------|-------------------|
| 5000 | ~5ms | 6x mais rápido |
| 5500 | ~1.8ms | 16x mais rápido |
| 6000 | 1ms | 30x mais rápido |

### O que consome os 29.77ms?

Estimativa de breakdown no Mac Mini (Haswell, 2.6 GHz):
- **Swoole request parsing:** ~2-5ms (HTTP parse, buffer copy)
- **PHP overhead:** ~5-10ms (json_decode, array access, FFI setup)
- **C IVF computation:** ~0.012ms (12μs — trivial!)
- **Swoole response:** ~2-5ms
- **Network (Docker bridge):** ~1-3ms
- **HAProxy:** ~1-2ms

O gargalo NÃO é a computação — é o stack overhead do PHP + Swoole + network!

### Por que yyjson deve ajudar?
Elimina o passo "PHP overhead" inteiro. O request vai direto do Swoole para C como raw string, e volta como um int. Potencial de cortar 5-10ms do p99.
