# ============================================================
# Stage 1: Download reference files
# ============================================================
FROM alpine:3.19 AS downloader

RUN apk add --no-cache curl
WORKDIR /resources

RUN curl -L -o references.json.gz \
    https://github.com/zanfranceschi/rinha-de-backend-2026/raw/main/resources/references.json.gz

# ============================================================
# Stage 2: Build IVF index (k-means clustering)
# ============================================================
FROM python:3.11-slim AS indexer

WORKDIR /build
COPY indexer/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY indexer/build_index.py .
COPY --from=downloader /resources/references.json.gz /resources/

ARG N_CLUSTERS=1500
RUN python build_index.py /resources/references.json.gz /index ${N_CLUSTERS}

# ============================================================
# Stage 3: Compile C vector search library
# ============================================================
FROM gcc:13-bookworm AS builder

WORKDIR /build
COPY src/vector_search.h src/vector_search.c ./
RUN gcc -O3 -msse2 -shared -fPIC -o libvector.so vector_search.c

# ============================================================
# Stage 4: Runtime
# ============================================================
FROM phpswoole/swoole:php8.3

# Install dependencies for FFI and compile it
RUN apt-get update && apt-get install -y libffi-dev && rm -rf /var/lib/apt/lists/*
RUN docker-php-ext-install ffi

# Set FFI to allow preloading
RUN echo "ffi.enable=true" >> /usr/local/etc/php/conf.d/ffi.ini

WORKDIR /app

# Copy IVF index binary files
COPY --from=indexer /index /data/index

# Copy C shared library
COPY --from=builder /build/libvector.so /app/src/libvector.so

# Copy PHP source
COPY src/ /app/src/

# Environment defaults
ENV INDEX_DIR=/data/index
ENV LIB_PATH=/app/src/libvector.so
ENV NPROBE=10
ENV PORT=9999
ENV WORKERS=2

EXPOSE 9999

CMD ["php", "/app/src/server.php"]
