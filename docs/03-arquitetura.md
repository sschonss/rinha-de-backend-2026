# 03 — Arquitetura da Solução

## Visão Geral

```
                    ┌──────────────────────────────────────────────┐
                    │           Docker Network "rinha"              │
                    │                                              │
  k6 (900 req/s)   │   ┌───────────┐                              │
  ────────────────► │   │  HAProxy  │                              │
  :9999             │   │  (25 MB)  │                              │
                    │   └─────┬─────┘                              │
                    │         │ round-robin                        │
                    │    ┌────┴────┐                               │
                    │    │         │                               │
                    │    ▼         ▼                               │
                    │ ┌──────┐ ┌──────┐                            │
                    │ │API-1 │ │API-2 │     ┌──────────────────┐   │
                    │ │162MB │ │162MB │────►│ IVF Index (mmap) │   │
                    │ │Swoole│ │Swoole│     │ 160MB shared     │   │
                    │ └──────┘ └──────┘     └──────────────────┘   │
                    │                                              │
                    └──────────────────────────────────────────────┘
```

---

## Fluxo de uma Request

```
1. k6 envia POST /fraud-score com JSON (~500 bytes)
       │
2. HAProxy recebe e faz round-robin para API-1 ou API-2
       │
3. Swoole worker recebe a request
       │
4. PHP chama VectorSearch::processRequest($rawBody) via FFI
       │  (ÚNICO call — raw JSON string + length)
       │
5. [C] yyjson parseia o JSON em ~500ns
       │
6. [C] Extrai campos e vetoriza (14 dimensões normalizadas)
       │
7. [C] Busca IVF:
       │   a. Calcula distância para 4000 centroids (AVX2)
       │   b. Seleciona top 5 clusters mais próximos
       │   c. Busca KNN nos ~3750 vetores desses clusters (AVX2)
       │   d. Retorna labels dos 5 vizinhos mais próximos
       │
8. [C] Conta fraud_count = sum(labels)
       │
9. PHP usa lookup table para resposta pré-computada:
       │   RESPONSES[fraud_count] → '{"approved":true,"fraud_score":0.2}'
       │
10. Swoole envia resposta HTTP
```

**Tempo total estimado no Mac Mini:** ~10-30ms por request

---

## Docker Multi-Stage Build

```dockerfile
# Stage 1: Download (nativo no host)
FROM --platform=$BUILDPLATFORM python:3.11-slim AS downloader
# Baixa os 3M vetores de referência (~600MB zip)

# Stage 2: Index Builder (nativo no host)  
FROM --platform=$BUILDPLATFORM python:3.11-slim AS indexer
# Roda K-means com 4000 clusters
# Gera: centroids.bin, vectors.bin, labels.bin, offsets.bin

# Stage 3: C Compiler (x86_64 target)
FROM gcc:13-bookworm AS builder
# Compila libvector.so com AVX2 flags
# IMPORTANTE: yyjson.c compilado SEPARADAMENTE (sem -ffast-math)

# Stage 4: Runtime
FROM phpswoole/swoole:php8.3
# Copia: índice + libvector.so + PHP sources
```

### Por que `--platform=$BUILDPLATFORM`?

O desenvolvimento é feito em **Mac ARM (M1/M2)**, mas o target é **x86_64 (Haswell)**.

- O downloader e indexer rodam **nativamente no ARM** (são apenas Python + NumPy)
- O builder compila **para x86** usando o GCC nativo do container
- Se tentássemos rodar K-means de 4000 clusters em QEMU (emulando x86 em ARM), levaria horas e crashava com segfault

---

## Estrutura de Arquivos

```
rinha-de-backend-2026/
├── Dockerfile                 # Multi-stage (4 stages)
├── docker-compose.yml         # HAProxy + 2× API
├── config/
│   └── haproxy.cfg           # Load balancer config
├── src/
│   ├── server.php            # Swoole HTTP server (minimal)
│   ├── VectorSearch.php      # FFI wrapper
│   ├── FraudDetector.php     # Vectorization logic (backup)
│   ├── vector_search.c       # HOT PATH — AVX2 SIMD + IVF + yyjson
│   ├── vector_search.h       # C header
│   ├── yyjson.c              # yyjson 0.10 (fastest JSON parser)
│   └── yyjson.h              # yyjson header
├── tests/
│   ├── test_search.c         # Unit test for C search
│   └── test_vectorization.php # Unit test for PHP vectorization
├── test/
│   ├── test.js               # k6 load test script
│   ├── smoke.js              # Smoke test
│   └── test-data.json        # 54K test entries with labels
├── docs/                     # Esta documentação
└── Makefile                  # Comandos úteis
```

---

## IVF Index — Estrutura em Disco

```
/data/index/
├── centroids.bin    # 4000 × 14 × float32 = 224 KB
├── vectors.bin      # 3M × 14 × float32 = 160 MB
├── labels.bin       # 3M × uint8 = 3 MB
└── offsets.bin      # 4000 × 2 × uint32 = 32 KB
                     # (start_idx, count) por cluster
```

### Como o mmap funciona:

```c
// No ivf_init():
g_idx.vectors = mmap(NULL, size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
```

1. `mmap` cria um mapping virtual — não copia dados para RAM
2. `MAP_POPULATE` diz ao kernel para pre-carregar todas as páginas
3. `MAP_PRIVATE` + read-only = kernel pode compartilhar as mesmas páginas físicas entre processos
4. **Resultado:** API-1 e API-2 usam a MESMA memória física para o índice!

Sem isso, cada instância precisaria de 163 MB (160 MB índice + 3 MB labels) = 326 MB > 324 MB disponível. **Impossível sem mmap compartilhado.**

---

## HAProxy Config

```
defaults
    mode http
    timeout connect 5s
    timeout client  30s
    timeout server  30s

frontend http
    bind *:9999
    default_backend api

backend api
    balance roundrobin
    option httpchk GET /ready
    server api1 api-1:9999 check
    server api2 api-2:9999 check
```

- Round-robin simples entre as 2 instâncias
- Health check em `/ready` — garante que o índice está carregado
- Timeouts conservadores (30s) para não matar requests em carga

---

## Swoole Server (server.php)

O server PHP é **ultra-minimal** — apenas routing e delegação para C:

```php
$server->on('request', function ($req, $res) use (&$ready) {
    if ($uri === '/ready') { /* health check */ }
    
    if ($uri === '/fraud-score') {
        $body = $req->rawContent();
        $fraudCount = VectorSearch::processRequest($body);  // ÚNICO FFI call
        $res->end(RESPONSES[$fraudCount]);  // lookup table, zero concatenação
    }
});
```

**Otimizações no PHP:**
- `RESPONSES` é uma constante array — pré-computada no parse time
- Zero `json_decode` — o raw body vai direto para C
- Zero `json_encode` — respostas são strings literais
- Zero objetos criados por request
- 1 worker por instância (sem context switching)

---

## FFI Interface

```php
self::$ffi = \FFI::cdef("
    int ivf_init(const char *index_dir, int nprobe);
    int ivf_process_request(const char *json_body, size_t json_len);
    void ivf_destroy(void);
", $libPath);
```

Uma única chamada FFI faz tudo:
- Parse JSON (yyjson)
- Vetorização (14 dims)
- Busca IVF (centroid distance + cluster search)
- Contagem de fraudes
- Retorna apenas um `int` (0-5)
