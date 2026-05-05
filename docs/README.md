# 📚 Documentação — Rinha de Backend 2026

## Participante: sschonss (Luiz Schons)

Esta documentação detalha toda a jornada de desenvolvimento da nossa solução para a **Rinha de Backend 2026** — uma competição brasileira de backend focada em detecção de fraudes via busca vetorial.

---

## Índice dos Documentos

| # | Documento | Descrição |
|---|-----------|-----------|
| 1 | [01-analise-competicao.md](./01-analise-competicao.md) | Análise da competição, regras, scoring e dataset |
| 2 | [02-decisoes-de-design.md](./02-decisoes-de-design.md) | Brainstorming, escolhas de tecnologia e trade-offs |
| 3 | [03-arquitetura.md](./03-arquitetura.md) | Arquitetura da solução, diagramas e fluxo de dados |
| 4 | [04-implementacao.md](./04-implementacao.md) | Detalhes da implementação em PHP+Swoole+C FFI |
| 5 | [05-otimizacoes.md](./05-otimizacoes.md) | Jornada de otimização: SSE2 → AVX2 → yyjson |
| 6 | [06-bug-ffast-math.md](./06-bug-ffast-math.md) | Post-mortem: como `-ffast-math` destruiu tudo |
| 7 | [07-resultados.md](./07-resultados.md) | Resultados oficiais e métricas de performance |

---

## Resumo da Jornada

```
Dia 1: Análise → Design → Implementação completa (13 tasks)
       Score local: -3813 (tudo dando timeout no QEMU)

Dia 1: Otimização Round 1 (SSE2 → AVX2, combined C function)
       Score local: +1322

Dia 1: Submissão oficial no Mac Mini (Haswell nativo)
       ⭐ Score oficial: +4526.21 (detecção PERFEITA)

Dia 1: Otimização Round 2 (yyjson — eliminar PHP json_decode)
       Score oficial: -6000 💀 (64% HTTP errors)

Dia 1: Debug + Fix (-ffast-math + get_num + null safety)
       Aguardando resultado... 🤞
```

---

## Tecnologias Utilizadas

- **PHP 8.3 + Swoole** — servidor HTTP assíncrono
- **C (FFI)** — kernel de busca vetorial com AVX2 SIMD
- **yyjson** — parser JSON em C (mais rápido do mundo)
- **HAProxy** — load balancer
- **IVF (Inverted File Index)** — busca aproximada em 3M vetores
- **Docker** — containerização multi-stage
- **k6** — load testing

## Ambiente da Competição

- Mac Mini Late 2014 — Intel i5-4278U (Haswell, 2.6 GHz)
- 8GB RAM, Ubuntu 24.04
- Limites: **1 CPU, 350MB RAM** total
- k6: 120s ramp até 900 req/s (~54K requests)
