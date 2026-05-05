# 08 - Jornada de Debugging e Análise de Competidores

## Contexto

Após o score inicial de **4526.21** (test #1263), todas as builds subsequentes falharam
com ~1000-2500 erros HTTP e p99 ≈ 2001ms (timeout do k6). Este documento registra toda
a investigação e as descobertas.

---

## Cronologia de Testes

| Teste  | Imagem | Mudança Principal                | Erros | p99     | Score    |
|--------|--------|----------------------------------|-------|---------|----------|
| #1263  | original | Build inicial                  | 0     | 29ms    | **4526** |
| #1425  | rollback | JIT=0, rollback pós-yyjson     | 2577  | timeout | -3776    |
| #1440  | v3     | --no-cache rebuild               | 1638  | timeout | -3513    |
| #1445  | v4     | Pin Swoole 6.2.0                 | 1138  | 2001ms  | -3330    |
| #1457  | v5     | Swoole 6.1.8 + sklearn 1.6.1     | 1023  | 2001ms  | -3236    |
| #1466  | v6     | Container único 325MB            | REJEITADO | -   | -        |
| #1498  | v7     | Assimétrico 300+25MB             | 5615  | 2001ms  | -6000    |
| #1508  | v8     | Quantização uint8                | 2011  | 2001ms  | -3667    |

---

## Hipóteses Testadas e Resultados

### 1. Regressão de versão do Swoole
- **Problema:** A imagem `phpswoole/swoole:php8.3` foi atualizada em 05/05 para Swoole 6.2.1
- **Teste:** Pinar em `phpswoole/swoole:6.2.0-php8.3` (v4) e depois `6.1.8-php8.3` (v5)
- **Resultado:** Melhoria marginal (2577 → 1138 → 1023 erros). **NÃO é a causa raiz.**

### 2. JIT do PHP desabilitado pelo Swoole
- **Descoberta:** Swoole auto-desabilita JIT ("JIT is incompatible with third party extensions that setup user opcode handlers")
- **Implicação:** `opcache.jit=1255` NUNCA funcionou, nem na build original que fez 4526
- **Conclusão:** **NÃO é a causa.** O score 4526 foi obtido SEM JIT.

### 3. Versão do scikit-learn
- **Problema:** `scikit-learn` atualizou de 1.6.x para 1.8.0, gerando clusters diferentes
- **Teste:** Pinar `scikit-learn==1.6.1`
- **Resultado:** Clusters diferentes mas qualidade similar. **Fator menor.**

### 4. Pressão de memória (168MB > 162MB)
- **Teoria:** O índice float32 (168MB) excede o limite do container (162MB), causando thrashing de page cache
- **Teste uint8:** Reduziu índice para 48MB, cabe em 162MB → Ainda falha!
- **Teste container único:** 325MB → REJEITADO pela competição ("requires at least two API instances")
- **Teste assimétrico:** 300MB + 25MB → Pior resultado (-6000)
- **Conclusão:** **NÃO é a causa única**, mas contribui.

### 5. MAP_POPULATE
- **Teste:** Com e sem MAP_POPULATE com índice de 48MB
- **Resultado:** Sem diferença significativa. **NÃO é a causa.**

### 6. Quantização uint8
- **Implementação:** Vetores float32[14] → uint8[16] com scaling per-dimension
- **Redução:** 168MB → 48MB
- **Problema:** Perdeu accuracy significativa (FP: 9→263, FN: 5→289)
- **Score:** -3667 (PIOR que float32). **Direção errada.**

### 7. O mistério não resolvido
- O MESMO approach (PHP+Swoole+C+IVF+mmap) fez 4526 uma vez e NUNCA mais
- Toda rebuild falha, mesmo com o mesmo código
- Testes locais SEMPRE passam (54100/54100, avg=3.4ms)
- A imagem Docker original foi sobrescrita no Hub — **PERDIDA**
- **Hipótese:** As imagens base (PHP, Swoole, GCC) mudaram entre a build original e as rebuilds

---

## Análise do Competidor Top: Demians12 (Score 3653, 0 erros)

### Stack
- **Linguagem:** Go (compilado, zero overhead de runtime)
- **HTTP:** fasthttp (ultra-performático)
- **JSON:** jsoniter (json-iterator, mais rápido que encoding/json)
- **Quantização:** int8 com scale=127, mapeamento [0,1] → [0,127]
- **IVF:** 256 clusters, nProbe=1
- **Index:** Arquivo único `index.bin` com magic header, centroids int16, vectors int8
- **Distância:** Assembly x86-64 otimizado (dist_amd64.s)
- **Memória:** ~50MB index (3M × 16 bytes + metadata)

### Configuração Docker
```yaml
cpus: "0.40"       # API (nós: 0.475)
mem_limit: "150MB"  # API (nós: 162MB)
cpus: "0.20"        # HAProxy (nós: 0.05)
mem_limit: "50MB"   # HAProxy (nós: 25MB)
```

### Comunicação: Unix Sockets (não TCP!)
```yaml
volumes:
  - rinha-sockets:/sockets
# HAProxy -> API via unix@/sockets/api.sock
```

### HAProxy: Modo TCP (não HTTP!)
```
mode tcp
timeout connect 50ms
timeout client 2s
timeout server 2s
server api1 unix@/sockets/api1.sock
```

### 🔑 INSIGHT CRÍTICO: Cheap Rules (Regras Baratas)

O segredo principal do competidor:

```go
const (
    ambigMin = 4   // score < 4 → NÃO É FRAUDE (skip KNN)
    ambigMax = 17  // score > 17 → É FRAUDE (skip KNN)
)

// Só faz KNN para casos ambíguos!
if score < ambigMin {
    fraudCount = 0     // approved, zero cost
} else if score > ambigMax {
    fraudCount = 5     // fraud, zero cost
} else {
    fraudCount = knnSearch(&v)  // KNN apenas para ambíguos
}
```

Eles calculam um "score" baseado em regras simples (21 features binárias):
- `amount >= 2000` → +1
- `installments >= 6` → +1
- `amountVsAvg >= 8` → +1
- `hour < 7` → +1
- `kmFromHome >= 200` → +1
- `txCount24h >= 8` → +1
- `isOnline` → +1
- `!cardPresent` → +1
- `!knownMerchant` → +1
- `mccRisk >= 95` → +1
- etc.

Se o score é óbvio (< 4 ou > 17), **pula KNN inteiramente**. Estimativa: ~60-70%
dos requests não precisam de KNN.

### IVF Repair (Busca Adaptativa)
Após buscar nProbe=1 cluster, verifica se outros clusters poderiam ter vizinhos
mais próximos usando bounding box lower bounds. Dá melhor accuracy sem scan completo.

### Pooling de Objetos
```go
var payloadPool = sync.Pool{
    New: func() any { return &Payload{} },
}
```

---

## Diferenças Fundamentais: Nossa Solução vs Top Scorer

| Aspecto | Nós (PHP+Swoole) | Top Scorer (Go) |
|---------|-------------------|-----------------|
| Linguagem | PHP 8.3 interpretado | Go compilado nativo |
| HTTP | Swoole HTTP Server | fasthttp |
| JSON | json_decode() nativo | jsoniter |
| FFI overhead | PHP → C via FFI | Nenhum (tudo em Go) |
| Cheap rules | ❌ Sempre faz KNN | ✅ Skip 60-70% dos KNN |
| Quantização | float32 (168MB) | int8 (48MB) |
| IVF clusters | 4000 | 256 |
| nProbe | 5 | 1 |
| IVF repair | ❌ | ✅ (bbox lower bound) |
| Comunicação | TCP (HAProxy → API) | Unix sockets |
| HAProxy mode | HTTP | TCP |
| Startup sync | service_started | service_healthy |
| Distância | AVX2 C auto-vectorized | Assembly x86-64 |
| Object pool | ❌ | ✅ sync.Pool |

---

## Decisões Tomadas para v9

### 1. Voltar para float32 (reverter uint8)
- uint8 piorou accuracy demais (FP: 9→263, FN: 5→289)
- float32 com 168MB não cabe em memória, MAS com cheap rules,
  a maioria dos requests nem precisa acessar o índice

### 2. Unix sockets
- Elimina overhead TCP (SYN/ACK, Nagle, buffer copies)
- HAProxy → API via `/sockets/api{1,2}.sock`

### 3. HAProxy TCP mode
- Sem parsing HTTP no HAProxy
- Timeout connect 50ms (era 5s)
- maxconn 8192 (era 1024)

### 4. service_healthy
- HAProxy só inicia quando APIs estão healthy
- Elimina race condition no startup

### 5. Implementar Cheap Rules (TODO)
- Calcular score baseado em regras simples
- Se óbvio (< threshold ou > threshold), retornar sem KNN
- Pode reduzir KNN de 100% → 30-40% dos requests

### 6. Reduzir clusters de 4000 → 1500
- Menos centroides para buscar na fase coarse
- Clusters maiores mas com nProbe menor

---

## Próximos Passos

1. ✅ Implementar Unix sockets
2. ✅ HAProxy TCP mode
3. ✅ service_healthy
4. ⬜ Implementar cheap rules no FraudDetector
5. ⬜ Reduzir nProbe de 5 → 3
6. ⬜ Testar v9 no Mac Mini
7. ⬜ Considerar rewrite em Go se PHP continuar falhando
