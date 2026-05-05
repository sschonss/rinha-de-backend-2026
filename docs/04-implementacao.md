# 04 — Implementação

## Plano de Execução

A implementação foi dividida em **13 tasks** executadas sequencialmente por sub-agentes especializados:

| # | Task | Descrição |
|---|------|-----------|
| 1 | project-structure | Criar estrutura de diretórios e arquivos base |
| 2 | ivf-c-library | Implementar busca vetorial IVF em C com SIMD |
| 3 | php-ffi-wrapper | Wrapper PHP para chamar a biblioteca C |
| 4 | fraud-detector | Lógica de vetorização e detecção em PHP |
| 5 | swoole-server | Servidor HTTP com Swoole |
| 6 | haproxy-config | Configuração do load balancer |
| 7 | dockerfile | Multi-stage build com indexer |
| 8 | docker-compose | Orquestração dos containers |
| 9 | index-builder | Script Python para gerar IVF index (K-means) |
| 10 | integration-tests | Testes de integração |
| 11 | unit-tests | Testes unitários |
| 12 | documentation | Documentação do projeto |
| 13 | makefile | Makefile com comandos úteis |

**Tempo total: ~2 horas** (incluindo debugging)

---

## Detalhes — Biblioteca C (vector_search.c)

### Estruturas de Dados

```c
typedef struct {
    float    *centroids;    // [n_clusters × 14] — centroids dos clusters
    float    *vectors;      // [n_vectors × 14]  — todos os 3M vetores
    uint8_t  *labels;       // [n_vectors]        — 0=legit, 1=fraud
    uint32_t *offsets;      // [n_clusters × 2]   — (start, count) por cluster
    uint32_t  n_vectors;    // 3.000.000
    uint32_t  n_clusters;   // 4.000
    uint32_t  n_dims;       // 14
    int       nprobe;       // 5
    void   *mmap_ptrs[5];   // ponteiros mmap para cleanup
    size_t  mmap_sizes[5];
    int     n_mmaps;
} IVFIndex;

static IVFIndex g_idx;  // Singleton global (shared por todos os requests)
```

### Função de Distância (AVX2)

```c
static inline float dist_euclidean(const float *restrict a, const float *restrict b) {
    // AVX2: processa dims 0-7 (8 floats em paralelo)
    __m256 va = _mm256_loadu_ps(a);
    __m256 vb = _mm256_loadu_ps(b);
    __m256 d = _mm256_sub_ps(va, vb);
    __m256 sq = _mm256_mul_ps(d, d);

    // SSE: processa dims 8-11 (4 floats)
    __m128 va2 = _mm_loadu_ps(a + 8);
    __m128 vb2 = _mm_loadu_ps(b + 8);
    __m128 d2 = _mm_sub_ps(va2, vb2);
    __m128 sq2 = _mm_mul_ps(d2, d2);

    // Horizontal sum do AVX2
    __m128 lo = _mm256_castps256_ps128(sq);
    __m128 hi = _mm256_extractf128_ps(sq, 1);
    __m128 sum128 = _mm_add_ps(_mm_add_ps(lo, hi), sq2);
    sum128 = _mm_hadd_ps(sum128, sum128);
    sum128 = _mm_hadd_ps(sum128, sum128);

    // Scalar: dims 12-13
    float d12 = a[12] - b[12], d13 = a[13] - b[13];
    return _mm_cvtss_f32(sum128) + d12*d12 + d13*d13;
}
```

**Performance:** ~3-4 ciclos de clock por distância (14 dims). Com Haswell a 2.6 GHz, isso é ~1.5ns por cálculo de distância.

### Busca IVF

```c
int ivf_search(const float *query, int *out_labels, float *out_distances, int k) {
    // 1. Encontra top-nprobe centroids mais próximos
    //    (insertion sort parcial — O(n_clusters × nprobe))
    for (uint32_t c = 0; c < n_clusters; c++) {
        float d = dist_euclidean(query, centroids + c * 14);
        // sorted insertion no array de top-5
    }

    // 2. Busca KNN nos clusters selecionados (max-heap)
    HeapItem heap[5]; int heap_size = 0;
    for (int p = 0; p < nprobe; p++) {
        int c = cent_ids[p];
        uint32_t start = offsets[c * 2];
        uint32_t count = offsets[c * 2 + 1];
        for (uint32_t i = 0; i < count; i++) {
            float d = dist_euclidean(query, vectors + (start+i) * 14);
            heap_push(heap, &heap_size, 5, d, start+i);
        }
    }

    // 3. Retorna labels dos K vizinhos mais próximos
    for (int i = 0; i < k; i++)
        out_labels[i] = labels[heap[i].idx];
}
```

**Complexidade por request:**
- Centroid search: 4000 distâncias = 4000 × 1.5ns = 6μs
- Cluster search: ~3750 distâncias = 3750 × 1.5ns = 5.6μs
- **Total computação:** ~12μs

### Full Pipeline (ivf_process_request)

```c
int ivf_process_request(const char *json_body, size_t json_len) {
    // 1. Parse JSON com yyjson (~500ns)
    yyjson_doc *doc = yyjson_read(json_body, json_len, 0);
    
    // 2. Extrai campos (safe number getter para int/real)
    float amount = (float)get_num(yyjson_obj_get(tx, "amount"));
    // ... 13 outros campos
    
    // 3. Vetoriza + Busca + Conta
    return ivf_fraud_score(amount, installments, ...);
}
```

---

## Detalhes — Index Builder (Python)

```python
# build_index.py (roda no Docker build time)
import numpy as np
from sklearn.cluster import MiniBatchKMeans

# Carrega 3M vetores
vectors = np.fromfile('vectors.bin', dtype=np.float32).reshape(-1, 14)
labels = np.fromfile('labels.bin', dtype=np.uint8)

# K-means com 4000 clusters
kmeans = MiniBatchKMeans(n_clusters=4000, batch_size=10000, n_init=3)
assignments = kmeans.fit_predict(vectors)

# Reordena vetores por cluster (contiguidade em memória)
order = np.argsort(assignments)
vectors_sorted = vectors[order]
labels_sorted = labels[order]

# Salva index binário
kmeans.cluster_centers_.tofile('centroids.bin')
vectors_sorted.tofile('vectors.bin')
labels_sorted.tofile('labels.bin')
# offsets: (start_idx, count) para cada cluster
offsets.tofile('offsets.bin')
```

**Tempo de build:** ~3-5 minutos no ARM nativo (com `--platform=$BUILDPLATFORM`)

---

## Detalhes — Vetorização

### Tabela MCC Risk

```c
static const MccEntry MCC_TABLE[] = {
    {"4511", 0.35f},  // Airlines
    {"5311", 0.25f},  // Department stores
    {"5411", 0.15f},  // Grocery stores
    {"5812", 0.30f},  // Restaurants
    {"5912", 0.20f},  // Drug stores
    {"5944", 0.45f},  // Jewelry
    {"5999", 0.50f},  // Misc specialty
    {"7801", 0.80f},  // Government lottery
    {"7802", 0.75f},  // Horse racing
    {"7995", 0.85f},  // Gambling
};
// MCCs não listados → 0.5 (risk médio)
```

### Parser de Datetime (Sakamoto's Algorithm)

```c
static void parse_datetime(const char *s, size_t len, int *hour, int *dow) {
    // Extrai hora diretamente da posição no ISO8601
    *hour = (s[11] - '0') * 10 + (s[12] - '0');
    
    // Day of week via Tomohiko Sakamoto
    int y = parse_year(s), m = parse_month(s), d = parse_day(s);
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    int w = (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
    *dow = (w + 6) % 7;  // Converte: 0=Mon,...,6=Sun
}
```

Elimina completamente `gmdate('N')` do PHP — zero overhead.

---

## Testes

### Testes de Integração (7/7 PASS)
1. Health check (`GET /ready`)
2. Request simples de fraude
3. Request legítima
4. Transação sem `last_transaction`
5. Edge case: amount = 0
6. Merchant desconhecido
7. Múltiplas requests concorrentes

### Testes Unitários (4/4 PASS)
1. Vetorização — valores normalizados corretos
2. MCC risk lookup — todos os MCCs da tabela
3. Day of week — datas conhecidas
4. Distance function — resultado matemático correto

### Load Test (k6)
- Script oficial da competição
- 54.100 requests em 120 segundos
- Valida aprovação/rejeição contra labels esperados
- Calcula score final com mesma fórmula da competição
