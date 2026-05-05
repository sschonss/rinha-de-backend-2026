# 02 — Decisões de Design

## Processo de Brainstorming

Antes de escrever qualquer código, passamos por um processo estruturado de brainstorming analisando diferentes abordagens e trade-offs.

---

## Pergunta 1: Linguagem Principal

### Opções Consideradas

| Linguagem | Prós | Contras |
|-----------|------|---------|
| **Go** | Goroutines, binário estático, ótima perf | Sem SIMD nativo fácil |
| **Rust** | Performance máxima, zero-cost abstractions | Complexidade, tempo de dev |
| **C/C++** | SIMD direto, controle total | Desenvolvimento lento, sem ecosystem web |
| **PHP + Swoole** | Rápido de desenvolver, async I/O, FFI para C | Overhead de linguagem interpretada |
| **Java/GraalVM** | JIT poderoso, ecosystem rico | JVM startup, memory overhead |

### Decisão: **PHP 8.3 + Swoole + C via FFI**

**Raciocínio:**
- PHP Swoole é surpreendentemente rápido para I/O-bound tasks
- O hot path (busca vetorial) roda 100% em C via FFI — zero overhead de PHP no caminho crítico
- FFI permite chamar código C diretamente sem serialização
- Desenvolvimento rápido — mais tempo para otimizar o que importa
- Swoole mantém workers persistentes (sem bootstrap por request)

---

## Pergunta 2: Algoritmo de Busca Vetorial

### O Desafio
Buscar os 5 vizinhos mais próximos em **3 milhões de vetores** com latência < 30ms.

Busca exaustiva (brute force): 3M × 14 dims × operações float = **muito lento**.

### Opções Consideradas

| Algoritmo | Complexidade | Precisão | Trade-off |
|-----------|-------------|----------|-----------|
| **Brute Force** | O(N) | 100% | Lento demais (3M vetores) |
| **HNSW** | O(log N) | ~99% | Muita RAM para grafos |
| **IVF (Inverted File)** | O(N/K × nprobe) | ~99% | Bom equilíbrio RAM/velocidade |
| **Product Quantization** | O(N) compressed | ~95% | Perda de precisão |
| **VP-Tree** | O(log N) | 100% | Pior caso O(N) |

### Decisão: **IVF (Inverted File Index)**

**Raciocínio:**
- Divide os 3M vetores em K clusters via K-means
- Na busca, só examina os `nprobe` clusters mais próximos
- Com K=4000 e nprobe=5: examina apenas ~3750 vetores ao invés de 3M (**800x redução!**)
- Precisão perfeita comprovada nos testes (0 FP, 0 FN)
- Usa mmap para mapear o índice direto do disco — compartilhado entre workers

---

## Pergunta 3: SIMD para Distance Computation

### O Hot Path
A função mais chamada é `dist_euclidean(a, b)` — calcula a distância L2 entre dois vetores de 14 floats. É chamada ~3750 vezes por request × 900 req/s = **3.375.000 vezes/segundo**.

### Evolução das implementações:

#### V1: Scalar (baseline)
```c
float dist = 0;
for (int i = 0; i < 14; i++) {
    float d = a[i] - b[i];
    dist += d * d;
}
```
→ 14 subtrações + 14 multiplicações + 14 somas = **42 operações**

#### V2: SSE2 (4 floats/ciclo)
```c
__m128 sum = _mm_setzero_ps();
for (int i = 0; i < 12; i += 4) {
    __m128 d = _mm_sub_ps(_mm_loadu_ps(a+i), _mm_loadu_ps(b+i));
    sum = _mm_add_ps(sum, _mm_mul_ps(d, d));
}
// + 2 scalar para dims 12-13
```
→ Processa 4 floats por ciclo. **~3.5x mais rápido**

#### V3: AVX2 (8 floats/ciclo) — VERSÃO FINAL
```c
// AVX2: dims 0-7 (8 floats de uma vez)
__m256 va = _mm256_loadu_ps(a);
__m256 vb = _mm256_loadu_ps(b);
__m256 d = _mm256_sub_ps(va, vb);
__m256 sq = _mm256_mul_ps(d, d);

// SSE: dims 8-11 (4 floats)
__m128 va2 = _mm_loadu_ps(a + 8);
__m128 vb2 = _mm_loadu_ps(b + 8);
__m128 d2 = _mm_sub_ps(va2, vb2);
__m128 sq2 = _mm_mul_ps(d2, d2);

// Scalar: dims 12-13
float d12 = a[12]-b[12], d13 = a[13]-b[13];
```
→ 8+4+2 = 14 dims em **apenas 3 etapas**. **~7x mais rápido que scalar**

### Decisão: **AVX2** (disponível no Haswell i5-4278U)

---

## Pergunta 4: Topologia de Deploy

### Opções

| Topologia | Prós | Contras |
|-----------|------|---------|
| HAProxy → 2× API | Simples, proven | Overhead do proxy |
| Nginx → 2× API | Mais features | Mais RAM |
| Custom proxy em Go | Ultra-leve | Tempo de dev |

### Decisão: **HAProxy 2.9 Alpine**

- Apenas 25 MB RAM
- 0.05 CPU
- Round-robin com health checks em `/ready`
- Configuração mínima (8 linhas)

---

## Pergunta 5: Gerenciamento de Memória

### O Problema
- 3M vetores × 56 bytes = **160 MB** de dados
- Cada instância da API tem apenas 162 MB
- Não dá para carregar uma cópia por instância!

### Decisão: **mmap com MAP_POPULATE**

```c
void *ptr = mmap(NULL, size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
```

- `mmap`: mapeia o arquivo direto na memória virtual
- `MAP_PRIVATE`: read-only, compartilhado pelo kernel entre processos
- `MAP_POPULATE`: pre-fault das páginas no startup (evita page faults durante requests)
- **Resultado**: as duas instâncias da API compartilham a mesma memória física do índice!

---

## Pergunta 6: Quantidade de Workers por Instância

### Análise
- Cada instância tem 0.475 CPU
- CPU-bound (cálculos de distância)
- Mais workers = mais context switching

### Decisão: **WORKERS=1**

Com apenas meio core, 1 worker por instância evita overhead de context switching. O Swoole já é event-driven para I/O.

---

## Pergunta 7: Parâmetros do IVF

### N_CLUSTERS (K)
- Regra empírica: `K = sqrt(N × nprobe)` = sqrt(3M × 5) ≈ **3873**
- Arredondamos para **4000**
- Mais clusters = cada cluster é menor = menos vetores por busca = mais rápido

### NPROBE
- Quantos clusters examinar por query
- nprobe=3: mais rápido, mas causa 307 FP + 127 FN (INACEITÁVEL)
- nprobe=5: **perfeito** — 0 FP, 0 FN no teste oficial
- nprobe=10: mais lento, mesma precisão

### Decisão: **K=4000, NPROBE=5**

---

## Resumo das Decisões

```
┌─────────────────────────────────────────────────────────────┐
│  STACK FINAL                                                │
├─────────────────────────────────────────────────────────────┤
│  Load Balancer:  HAProxy 2.9 Alpine (0.05 CPU, 25 MB)      │
│  API Server:     PHP 8.3 + Swoole (×2, 0.475 CPU, 162 MB)  │
│  Compute:        C library via FFI (AVX2 SIMD)              │
│  JSON Parser:    yyjson 0.10 (em C, single FFI call)        │
│  Index:          IVF (4000 clusters, nprobe=5, K=5)         │
│  Memory:         mmap + MAP_POPULATE (shared entre workers) │
│  Workers:        1 por instância                            │
└─────────────────────────────────────────────────────────────┘
```
