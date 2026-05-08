# ============================================================
# Stage 1: Download reference vectors (native arch)
# ============================================================
FROM --platform=$BUILDPLATFORM alpine:3.19 AS downloader
RUN apk add --no-cache curl
WORKDIR /resources
RUN curl -L -o references.json.gz \
    https://github.com/zanfranceschi/rinha-de-backend-2026/raw/main/resources/references.json.gz

# ============================================================
# Stage 2: Build IVF1 index (single index.bin, int16 quantized)
# ============================================================
FROM --platform=$BUILDPLATFORM python:3.11-slim AS indexer
WORKDIR /build
COPY indexer/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY indexer/build_index.py .
COPY --from=downloader /resources/references.json.gz /resources/

ARG N_CLUSTERS=4096
RUN python build_index.py /resources/references.json.gz /index ${N_CLUSTERS}

# ============================================================
# Stage 3: Compile C vector search lib (AVX2 + FMA, target=haswell amd64)
# ============================================================
FROM --platform=linux/amd64 gcc:13-bookworm AS builder
WORKDIR /build
COPY src/vector_search.h src/vector_search.c ./
RUN gcc -O3 -march=haswell -mavx2 -mfma -mbmi2 -mpopcnt \
        -ffast-math -funroll-loops -fno-plt \
        -shared -fPIC -o libvector.so vector_search.c -lm

# ============================================================
# Stage 4: Runtime — PHP 8.3 + Swoole 6.1.8 + FFI + opcache JIT (amd64)
# ============================================================
FROM --platform=linux/amd64 phpswoole/swoole:6.1.8-php8.3

RUN apt-get update && apt-get install -y --no-install-recommends libffi-dev \
 && rm -rf /var/lib/apt/lists/*
RUN docker-php-ext-install ffi

# FFI + opcache (JIT disabled — Swoole installs opcode handlers, JIT can't run)
RUN { \
      echo "ffi.enable=true"; \
      echo "memory_limit=32M"; \
      echo "opcache.enable=1"; \
      echo "opcache.enable_cli=1"; \
      echo "opcache.memory_consumption=24"; \
      echo "opcache.max_accelerated_files=2000"; \
      echo "opcache.validate_timestamps=0"; \
      echo "opcache.jit=disable"; \
      echo "opcache.jit_buffer_size=0"; \
      echo "realpath_cache_size=512K"; \
      echo "realpath_cache_ttl=600"; \
    } > /usr/local/etc/php/conf.d/zz-rinha.ini

WORKDIR /app

# Single-file IVF1 index
COPY --from=indexer /index/index.bin /data/index.bin

# C shared library
COPY --from=builder /build/libvector.so /app/src/libvector.so

# PHP source
COPY src/ /app/src/

ENV INDEX_PATH=/data/index.bin
ENV LIB_PATH=/app/src/libvector.so
ENV FAST_NPROBE=8
ENV FULL_NPROBE=24
ENV WORKERS=1
ENV WARMUP=500
ENV PORT=9999

EXPOSE 9999

CMD ["php", "/app/src/server.php"]
