# syntax=docker/dockerfile:1

# ---- Build stage ----
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      cmake \
      ninja-build \
      g++ \
      ca-certificates \
      git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Tests/benchmarks pull GoogleTest/Benchmark via FetchContent and aren't
# needed to run the CLI in the runtime image; skip them to keep the build
# fast and dependency-free.
RUN cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DDBENGINE_BUILD_TESTS=OFF \
      -DDBENGINE_BUILD_BENCHMARKS=OFF \
    && cmake --build build --parallel

# ---- Runtime stage ----
FROM debian:bookworm-slim AS runtime

RUN useradd --create-home --shell /usr/sbin/nologin dbengine \
    && mkdir -p /home/dbengine/data \
    && chown -R dbengine:dbengine /home/dbengine

COPY --from=build /src/build/src/dbengine_cli /usr/local/bin/dbengine_cli

USER dbengine
WORKDIR /home/dbengine

VOLUME ["/home/dbengine/data"]
ENTRYPOINT ["dbengine_cli"]
CMD ["/home/dbengine/data/dbengine.db"]
