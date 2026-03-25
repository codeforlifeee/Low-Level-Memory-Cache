# syntax=docker/dockerfile:1

FROM gcc:13-bookworm AS builder

WORKDIR /app

RUN apt-get update \
 && apt-get install -y --no-install-recommends cmake ninja-build \
 && rm -rf /var/lib/apt/lists/*

COPY . .

RUN cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_TESTS=OFF \
    -DENABLE_BENCHMARKS=OFF \
    -DENABLE_CLI=OFF \
    -DENABLE_HTTP=ON \
 && cmake --build build --target cache_http --parallel

FROM gcc:13-bookworm AS runtime

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/mini-redis
COPY --from=builder /app/build/cache_http /usr/local/bin/cache_http

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
  CMD ["curl", "-fsS", "http://127.0.0.1:8080/health"]

ENTRYPOINT ["/usr/local/bin/cache_http"]
CMD ["8080", "50000"]
