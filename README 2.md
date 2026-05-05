# Rinha de Backend 2026 — PHP/Swoole + C FFI

Detecção de fraude por busca vetorial usando IVF (Inverted File Index).

## Stack
- **API**: PHP 8.3 + Swoole (HTTP server async)
- **Busca vetorial**: C com SSE2 SIMD via PHP FFI
- **Indexação**: Python + scikit-learn (k-means offline)
- **Load balancer**: HAProxy

## Arquitetura
- HAProxy distribui round-robin para 2 instâncias PHP/Swoole
- IVF index pré-construído no Docker build (~1500 clusters)
- Dados compartilhados via mmap entre instâncias
- Limites: 1 CPU, 350MB RAM total

## Como rodar
```bash
docker compose up --build
curl http://localhost:9999/ready
```
