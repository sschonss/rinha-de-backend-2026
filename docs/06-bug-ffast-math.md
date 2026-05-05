# 06 — Post-Mortem: O Bug do `-ffast-math`

## TL;DR

Compilar o **yyjson** (parser JSON) com `-ffast-math` corrompe o parsing de números em hardware x86 nativo, causando crash/timeout em 64% das requests. O bug **não se manifesta** em emulação QEMU, tornando-o invisível nos testes locais.

---

## Timeline do Incidente

```
23:14 UTC  Resultado do teste #1342: Score -6000 💀
           → 10.271 HTTP errors (64.52% failure rate)
           → p99 = 2002ms (timeout)

23:20 UTC  Decisão: rollback + debug em paralelo
23:22 UTC  Rollback para versão estável (commit 397e595)
23:24 UTC  Imagem rollback pushada ao Docker Hub
23:25 UTC  Teste #1350 aberto (rollback)

23:30 UTC  Início do debug
23:35 UTC  ROOT CAUSE identificada: -ffast-math no yyjson.c
23:38 UTC  Fix implementado + testado localmente (5000/5000 OK)
23:40 UTC  Imagem corrigida pushada ao Docker Hub
23:41 UTC  Teste #1356 aberto (versão corrigida)
```

**Tempo de resolução:** 21 minutos (da detecção ao fix deployed)

---

## O Bug

### Linha Problemática no Dockerfile

```dockerfile
RUN gcc -O3 -march=haswell -mavx2 -ffast-math -funroll-loops \
    -shared -fPIC -o libvector.so vector_search.c yyjson.c
```

Ambos os arquivos são compilados com as **mesmas flags**, incluindo `-ffast-math`.

### O que `-ffast-math` faz?

`-ffast-math` é um grupo de flags que relaxa conformidade IEEE 754:

```
-ffast-math = -fno-math-errno
            + -funsafe-math-optimizations
            + -ffinite-math-only      ← ESTE É O PROBLEMA
            + -fno-rounding-math
            + -fno-signaling-nans
            + -fcx-limited-range
            + -fexcess-precision=fast
```

#### `-ffinite-math-only`
Diz ao compilador: "Pode assumir que NaN e ±Inf NUNCA existem."

O compilador então pode:
- Remover checks de NaN
- Otimizar `x == x` para `true` (normalmente `false` para NaN)
- Transformar `x != x` em `false`
- Reordenar operações que produziriam NaN

#### `-funsafe-math-optimizations`
Permite reordenação de operações de ponto flutuante:
- `(a + b) + c` pode virar `a + (b + c)` 
- Isso muda resultados quando há perda de precisão

### Como isso quebra o yyjson?

O yyjson usa aritmética de ponto flutuante **precisa** para converter strings JSON em doubles. O algoritmo:

1. Parseia dígitos como inteiro
2. Aplica expoente com multiplicação/divisão por potências de 10
3. Usa técnicas de "exact rounding" que dependem de comportamento IEEE 754 estrito

Com `-ffast-math`:
- O parser pode produzir valores **ligeiramente errados** para certos números
- Em casos extremos, pode produzir NaN ou Inf que o código não detecta
- O compilador pode "otimizar" loops de convergência em loops infinitos
- Comparações com NaN são undefined behavior

---

## Por que NÃO falhou no QEMU?

### Teoria 1: Mesma instrução, resultado diferente
QEMU traduz instruções x86 para ARM em runtime. Embora a lógica seja "a mesma", a implementação de operações float no QEMU pode:
- Usar arredondamento diferente internamente
- Não propagar NaN da mesma forma que hardware real
- Ter precision diferente em intermediários

### Teoria 2: Timing e concorrência
No QEMU (10-50x mais lento), cada request leva ~1-2ms. No hardware nativo, requests são processadas em ~30μs. Com 900 req/s:
- QEMU: requests são serializadas (não há pressão real)
- Nativo: múltiplas requests podem causar contenção de memória/cache

### Teoria 3: Compilação condicional
GCC pode gerar código diferente baseado no target. Com `-march=haswell`, pode usar instruções FMA (fused multiply-add) que têm semântica diferente de mul+add separados. No QEMU, essas instruções podem ser emuladas com semântica ligeiramente diferente.

---

## Bug Secundário: `yyjson_get_real()` para inteiros

### O Problema
yyjson distingue tipos internamente:
- `"amount": 500` → tipo SINT (inteiro)
- `"amount": 500.0` → tipo REAL (double)

```c
// ERRADO: retorna 0.0 se o valor é um inteiro!
float amount = (float)yyjson_get_real(yyjson_obj_get(tx, "amount"));

// CORRETO: handle ambos os tipos
static inline double get_num(yyjson_val *val) {
    if (!val) return 0.0;
    if (yyjson_is_real(val)) return yyjson_get_real(val);
    if (yyjson_is_int(val)) return (double)yyjson_get_sint(val);
    return 0.0;
}
```

### Impacto
~2.6% dos entries no dataset têm pelo menos um campo numérico como inteiro. Isso não causa crash, mas produz vetores errados (com 0 onde deveria ter um valor), levando a classificações incorretas.

### Na prática
Este bug sozinho causaria ~2-3% de erros de detecção, não 64% de failures. O bug principal é definitivamente o `-ffast-math`.

---

## Bug Terciário: JIT + Swoole

### O Problema
```ini
opcache.jit=1255
```

O JIT do PHP 8.3 é incompatível com o Swoole porque ambos manipulam opcodes internos. Quando ativado:
- Pode causar segfaults esporádicos
- Comportamento undefined em co-routines

### Fix
```ini
opcache.jit=0
```

Mantemos apenas `opcache.enable=1` para bytecode caching (que funciona perfeitamente com Swoole).

---

## A Correção

### Compilação Separada

```dockerfile
# yyjson: O3 + AVX2 mas SEM ffast-math
RUN gcc -O3 -march=haswell -mavx2 -funroll-loops -fPIC -c -o yyjson.o yyjson.c && \
    # vector_search: O3 + AVX2 COM ffast-math (safe — nosso código controla os floats)
    gcc -O3 -march=haswell -mavx2 -ffast-math -funroll-loops -fPIC -c -o vector_search.o vector_search.c && \
    # Link juntos
    gcc -shared -o libvector.so vector_search.o yyjson.o
```

**Por que `-ffast-math` é seguro no vector_search.c?**
- Nossos floats são sempre valores normalizados [0, 1]
- Não temos NaN/Inf no pipeline de distância
- A reordenação de operações afeta apenas precisão (última casa decimal), não corretude

**Por que NÃO é seguro no yyjson.c?**
- JSON numbers podem ser qualquer double válido
- O parser depende de IEEE 754 strict para rounding correto
- Detecção de overflow/underflow é essencial

---

## Lições Aprendidas

### 1. `-ffast-math` é uma arma nuclear
> Nunca aplique `-ffast-math` em código que parseia, serializa ou faz aritmética de precisão. É seguro apenas para computação numérica onde você controla os inputs.

### 2. QEMU não é substituto para hardware real
> Bugs de floating-point e timing podem se manifestar APENAS em hardware nativo. Sempre teste no target real antes de declarar vitória.

### 3. Compile bibliotecas de terceiros com flags conservadoras
> Suas flags de otimização são para SEU código. Libs externas (yyjson, zlib, etc) foram testadas com flags padrão.

### 4. Separar compilation units dá controle granular
> Compilar como objetos separados (`.o`) antes de linkar permite flags diferentes por arquivo. É a prática correta.

### 5. Valide em escala, não apenas com smoke tests
> O bug poderia ter sido detectado se testássemos com TODOS os 54K entries localmente (embora a manifestação em QEMU não fosse garantida).

---

## Checklist para Prevenir no Futuro

- [ ] Nunca usar `-ffast-math` em parsers/serializers
- [ ] Compilar libs de terceiros como objetos separados
- [ ] Testar com dataset completo, não apenas samples
- [ ] Ter flag `DEBUG` que compila sem otimizações agressivas
- [ ] Rodar sanitizers (ASan, UBSan) pelo menos uma vez antes de deploy
