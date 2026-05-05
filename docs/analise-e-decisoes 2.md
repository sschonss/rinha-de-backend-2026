# Rinha de Backend 2026 — Análise e Decisões de Design

Documento de registro de todo o processo de análise, perguntas, respostas e raciocínio que levaram ao design final da nossa solução.

---

## 1. Entendimento do Desafio

### O que é a Rinha de Backend 2026?
Uma competição amistosa de backend com o tema **detecção de fraudes por busca vetorial**. Cada participante constrói uma API sob restrições rígidas de CPU, memória e arquitetura.

### Requisitos do desafio
- **API**: dois endpoints na porta `9999`
  - `GET /ready` — health check (responder 2xx quando pronto)
  - `POST /fraud-score` — receber transação, devolver decisão de fraude
- **Lógica**: transformar payload em vetor de 14 dimensões, buscar os 5 vizinhos mais próximos em 3 milhões de vetores de referência, calcular `fraud_score = fraudes_nos_5 / 5`, aprovar se `< 0.6`
- **Arquitetura obrigatória**: load balancer + mínimo 2 instâncias de API (round-robin)
- **Restrições**: 1 CPU e 350MB RAM **no total** entre todos os containers
- **Entrega**: `docker-compose.yml` na branch `submission`, imagens `linux/amd64`
- **Pontuação**: soma de score de latência (p99, escala logarítmica) + score de detecção (precisão ponderada). Total de -6000 a +6000.

### Números críticos
- **3.000.000 de vetores** de referência com 14 dimensões cada
- Vetores brutos: 3M × 14 × 4 bytes (float32) = **168MB**
- Labels: 3M × 1 byte = **3MB**
- Ambiente de teste: Mac Mini Late 2014, 2.6 GHz, 8GB RAM, Ubuntu 24.04
- Timeout HTTP: 2001ms
- p99 ≤ 1ms = score máximo (3000 pts); p99 > 2000ms = score mínimo (-3000 pts)
- Cada 10x de melhoria no p99 = +1000 pontos

---

## 2. Perguntas e Decisões

### Pergunta 1: Qual linguagem/stack usar?

**Opções apresentadas:**
1. Rust (recomendado — melhor performance, ideal para desafios de latência)
2. Go (boa performance, mais simples de escrever)
3. Java/Kotlin (GraalVM nativo pode ter boa performance)
4. C/C++ (performance máxima, mais complexo)
5. Node.js/Bun (familiar para muitos devs)
6. Outra linguagem

**Decisão: PHP com Swoole**

**Raciocínio:**
- Swoole dá ao PHP capacidades de async/coroutines com performance significativa
- Escolha ousada que diferencia dos participantes tradicionais (Go, Rust, Java)
- Swoole tem HTTP server embutido, sem overhead de FPM/Apache
- Permite FFI para chamar código C nativo nos hot paths

---

### Pergunta 2: Qual estratégia de busca vetorial?

**Opções apresentadas:**
1. HNSW (Approximate Nearest Neighbor) — troca precisão mínima por velocidade enorme
2. VP-Tree (Vantage Point Tree) — busca exata, boa para 14 dimensões
3. KD-Tree — busca exata, clássica para dimensões baixas (<20)
4. Banco vetorial externo (Qdrant, pgvector) no docker-compose
5. Recomendação baseada nas restrições

**Decisão: Pedir recomendação baseada nas restrições**

**Análise feita para a recomendação:**

#### Análise de memória
- Vetores brutos: 168MB (fixo, independente do algoritmo)
- Com mmap, dados compartilhados entre 2 instâncias (conta 1x na RAM física)
- Orçamento: 350MB - 168MB = 182MB para tudo mais (nginx, 2 PHP processes, OS overhead, índice)

#### Análise de performance por algoritmo

| Algoritmo | Complexidade por query | p99 estimado | Memória extra | Precisão |
|-----------|----------------------|--------------|---------------|----------|
| Brute force SIMD | O(3M × 14) = 42M ops | 2-5ms | 0 | 100% |
| IVF (1500 clusters, 10 probes) | O(1500×14 + ~20K×14) = ~301K ops | 0.1-0.5ms | ~12MB | 98-99% |
| HNSW (M=4) | O(log N × 14) ≈ ~800 ops | <0.1ms | ~96MB (grafo) | 97-99% |

#### Cálculo detalhado — Brute Force
- 3M × 14 multiply-adds = 42M operações
- Com AVX2 (8 floats): ~5.25M vector ops
- CPU a ~2GHz, ~2 ops/cycle: **~2.6ms por query**
- Mais partial sort para top-5: +~0.5ms
- **Score p99 ≈ 2522 pontos** (bom, mas não ótimo)

#### Cálculo detalhado — IVF
- Distância a 1500 centroides: ~21K ops (desprezível)
- Busca em ~10 clusters × ~2000 vetores = 20K vetores × 14 = 280K ops
- **~0.15ms por query**
- **Score p99 ≈ 2800-3000 pontos** (quase máximo!)

#### Cálculo detalhado — HNSW
- ~20-40 cálculos de distância por query
- Cada cálculo: 14 ops
- **<0.1ms por query → Score = 3000 (máximo)**
- MAS: memória do grafo com M=4: 3M × 8 × 4 = 96MB extras → Total ~267MB
- Deixa apenas 83MB para nginx + 2 PHP processes — **muito apertado**

---

### Pergunta 3: Qual a prioridade principal?

**Opções apresentadas:**
1. Algo competitivo — máxima performance possível, mesmo que mais complexo
2. Equilíbrio — boa performance com código compreensível
3. Algo funcional — participar e aprender, sem obsessão com ranking

**Decisão: Máxima performance possível, competitivo**

---

### Decisão final de abordagem: 3 opções apresentadas

#### Opção 1 (Recomendada): IVF + C FFI + mmap
- **p99 esperado**: 0.1-0.5ms → ~2800-3000 pts de latência
- **Precisão**: 98-99%+ com 10-15 probes
- **Memória**: ~183MB compartilhada, confortável nos 350MB
- **Complexidade**: Média

#### Opção 2: HNSW via hnswlib + wrapper C
- **p99 esperado**: <0.1ms → 3000 pts
- **Precisão**: 97-99%
- **Memória**: ~267MB+ — risco de estourar 350MB
- **Complexidade**: Alta (wrapper C++ para PHP)

#### Opção 3: Brute Force SIMD + C FFI
- **p99 esperado**: 2-5ms → ~2300-2700 pts
- **Precisão**: 100% (busca exata)
- **Memória**: ~171MB (mínimo possível)
- **Complexidade**: Baixa

**Decisão: Opção 1 — IVF + C FFI + mmap**

**Justificativa:**
- Melhor custo-benefício: p99 sub-milissegundo com memória confortável
- Perda de precisão mínima (1-2%) compensada por ganho massivo de latência
- Cabe nos 350MB com folga para nginx + PHP workers
- Mais simples que HNSW, muito mais rápido que brute force

---

## 3. Design Detalhado

### Seção 1: Arquitetura & Docker Compose ✅ Aprovada

```
┌─────────────┐
│   cliente    │
└──────┬──────┘
       │ :9999
┌──────▼──────┐
│    nginx     │  (load balancer, round-robin)
│  cpus: 0.05  │
│  mem:  10MB  │
└──┬───────┬──┘
   │       │
┌──▼──┐ ┌──▼──┐
│api-1│ │api-2│  (PHP/Swoole HTTP servers)
│0.475│ │0.475│  CPU cada
│170MB│ │170MB│  memória cada
└──┬──┘ └──┬──┘
   │       │
   └───┬───┘
       │ mmap (leitura compartilhada)
┌──────▼──────┐
│  index.bin   │  (IVF index pré-construído)
│  ~183MB      │  (arquivo no volume compartilhado)
└─────────────┘
```

**Distribuição de recursos (total = 1 CPU, 350MB):**
- **nginx**: 0.05 CPU, 10MB
- **api-1**: 0.475 CPU, 170MB
- **api-2**: 0.475 CPU, 170MB

**Detalhe sobre mmap**: As duas instâncias fazem mmap do mesmo arquivo binário. O kernel compartilha as páginas físicas, então os ~183MB ocupam memória uma única vez. Páginas mmap'd de arquivo read-only são compartilhadas pelo kernel e geralmente não contam no cgroup memory limit da mesma forma.

---

### Seção 2: Pipeline de Dados (build time) — Em discussão

**Estratégia**: Dockerfile multi-stage.

**Stage 1 — "indexer" (Python + NumPy/scikit-learn):**
1. Descomprime `references.json.gz`
2. Converte 3M vetores para array float32 binário
3. Roda k-means (~1500 clusters)
4. Reorganiza vetores por cluster (contíguos na memória)
5. Gera arquivos binários:
   - `centroids.bin` (1500 × 14 × float32 = 84KB)
   - `vectors.bin` (3M × 14 × float32 = 168MB)
   - `labels.bin` (3M × uint8 = 3MB)
   - `cluster_offsets.bin` (1500 × 2 × uint32 = 12KB)
   - `meta.bin` (header com counts)

**Stage 2 — "runtime" (PHP + Swoole + libvector.so):**
- Copia os .bin do stage 1
- Compila a biblioteca C com SIMD
- PHP com ext-ffi habilitado

**Vantagem do formato binário por cluster**: vetores do mesmo cluster ficam contíguos na memória → cache-friendly, acesso via offset simples.

**Arquivos auxiliares**: `mcc_risk.json` e `normalization.json` hardcoded em PHP (poucos valores, evita I/O).

---

### Pergunta respondida: Por que ~1500 clusters?

Análise do trade-off:

| Clusters | Vetores/cluster | Probing 10 clusters | Tempo estimado | Precisão |
|----------|----------------|---------------------|---------------|----------|
| 500 | ~6000 | 60K vetores | ~0.4ms | 95-97% |
| **1500** | **~2000** | **20K vetores** | **~0.15ms** | **98-99%** |
| 3000 | ~1000 | 10K vetores | ~0.08ms | 96-98%* |
| 5000 | ~600 | 6K vetores | ~0.05ms | 93-96%* |

*Precisão cai com muitos clusters porque o k-means fica menos representativo.

**1500 é o sweet spot porque:**
- ~2000 vetores/cluster — tamanho que cabe em L1/L2 cache (~112KB)
- 10 probes = buscar em ~20K vetores (140x menos que brute force)
- Precisão 98-99% — perda quase imperceptível no score de detecção
- Centroides pequenos: 1500 × 14 × 4 bytes = 84KB (cabe inteiro em L1 cache)
- Valor tunável no build sem mudar código

**Decisão: ~1500 clusters, tunável nos testes** ✅

---

### Seção 3: Algoritmo IVF ✅ Aprovada

**Fluxo de uma query:**
1. Recebe JSON da transação
2. Vetoriza: extrai 14 campos e normaliza → `query[14]` (float32)
3. Busca centroides: distância do query a todos os 1500 centroides → top 10 mais próximos
4. Busca nos clusters: para cada um dos 10 clusters, distância a todos os vetores → mantém max-heap de tamanho 5 (top-5 global)
5. Conta fraudes: `fraud_score = fraudes_nos_5 / 5`
6. Responde: `approved = (fraud_score < 0.6)`

**Otimizações:**
- Distância euclidiana **sem raiz quadrada** (ordem relativa é a mesma)
- Max-heap de tamanho 5 em vez de sort completo — O(log 5) por inserção
- Hot path inteiro (busca IVF) é **uma única chamada FFI** ao C

**Divisão PHP/C:**
| Operação | Onde | Razão |
|----------|------|-------|
| Parse JSON | PHP | Swoole faz isso rápido |
| Vetorização (14 campos) | PHP | Poucas operações |
| Distância a centroides + busca clusters | **C via FFI** | Hot path, SIMD |
| Decisão + JSON response | PHP | Trivial |

---

### Seção 4: Biblioteca C (libvector.so) ✅ Aprovada

**Interface FFI:**
```c
int ivf_search(const float *query, int *out_labels, float *out_distances, int k);
int ivf_init(const char *index_dir);
void ivf_destroy();
```

**SIMD**: SSE2 (garantido em x86_64, compatível com Mac Mini Late 2014).
- Processa 4 floats por instrução
- 14 dims = 3 blocos SSE (12 dims) + 2 escalares
- Compilação: `gcc -O3 -msse2 -shared -fPIC -o libvector.so vector_search.c`

**Inicialização**: `ivf_init` faz mmap dos .bin. Com Swoole fork, mmap é herdado — zero cópia.

---

### Seção 5: API PHP/Swoole ✅ Aprovada

**Servidor Swoole HTTP** com:
- 2 workers por instância (decisão do usuário)
- FFI inicializado no `workerStart` (mmap compartilhado)
- Constantes de normalização e mcc_risk hardcoded (evita I/O)
- Vetorização em PHP puro (~14 operações simples com clamp)

---

### Pergunta: Workers por instância

**Análise:**
| Workers | CPU/worker | Prós | Contras |
|---------|-----------|------|---------|
| 1 | 0.475 | Zero context switch | Se 1 req trava, bloqueia |
| **2** | ~0.24 | Overlap JSON+FFI | Context switching |

**Decisão: 2 workers por instância** (preferência do usuário) ✅

---

### Pergunta: Load balancer — nginx vs alternativas

**Análise:**
| LB | Overhead RAM | Latência adicionada |
|----|-------------|-------------------|
| nginx | ~8-15MB | ~0.1-0.3ms |
| **HAProxy** | ~5-8MB | ~0.05-0.1ms |

HAProxy: feito especificamente para LB, overhead ~50% menor que nginx, ~0.05ms latência.

**Decisão: HAProxy** ✅

---

### Seção 7: Erros & Edge Cases ✅ Aprovada

**Estratégia**: Nunca retornar HTTP 500 (peso 5). Qualquer erro → `{"approved": true, "fraud_score": 0.0}` com HTTP 200.

**Edge cases:**
| Caso | Tratamento |
|------|-----------|
| `last_transaction: null` | dims 5 e 6 = -1.0 |
| MCC desconhecido | mcc_risk = 0.5 |
| `avg_amount = 0` | clamp(∞) = 1.0 |
| JSON malformado | Fallback approved:true |
| FFI crash | Try/catch → fallback |

---

## 4. Resumo Final de Decisões

| Decisão | Escolha | Justificativa |
|---------|---------|---------------|
| Linguagem | PHP + Swoole | Escolha do participante, Swoole dá async + HTTP server embutido |
| Busca vetorial | IVF (Inverted File Index) | Melhor custo-benefício: sub-ms com memória confortável |
| Clusters | ~1500, tunável | Sweet spot: 98-99% precisão, ~0.15ms por query |
| Kernel | C com SSE2 via PHP FFI | SIMD para hot path, uma chamada FFI por request |
| Dados | mmap compartilhado entre instâncias | 183MB contados 1x na RAM física |
| Workers | 2 por instância | Preferência do usuário, permite overlap |
| Load balancer | HAProxy | ~50% menos overhead que nginx |
| Error handling | Fallback defensivo | Nunca HTTP 500 (peso 5 no scoring) |
| Build | Dockerfile multi-stage (Python+C→PHP) | k-means offline, index pré-construído |

## 5. Scores Esperados

| Componente | Estimativa | Máximo |
|-----------|-----------|--------|
| Latência (p99 ~0.15-0.5ms) | 2800-3000 | 3000 |
| Detecção (98-99% precisão) | 2000-2800 | 3000 |
| **Total esperado** | **4800-5800** | **6000** |
