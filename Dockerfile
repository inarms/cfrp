# Build stage — runs natively on each target platform via QEMU
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    git \
    pkg-config \
    zip \
    unzip \
    curl \
    bash \
    perl \
    python3 \
    tar \
    autoconf \
    automake \
    libtool \
    linux-libc-dev \
    && rm -rf /var/lib/apt/lists/*

# Install vcpkg — shallow clone, pinned to builtin-baseline in vcpkg.json
WORKDIR /opt
COPY vcpkg.json /tmp/vcpkg.json
RUN VCPKG_COMMIT=$(python3 -c "import json; print(json.load(open('/tmp/vcpkg.json'))['builtin-baseline'])") && \
    git clone --depth 1 https://github.com/microsoft/vcpkg.git && \
    git -C vcpkg fetch --depth 1 origin ${VCPKG_COMMIT} && \
    git -C vcpkg checkout ${VCPKG_COMMIT} && \
    ./vcpkg/bootstrap-vcpkg.sh -disableMetrics

ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH="${PATH}:${VCPKG_ROOT}"

WORKDIR /app
COPY vcpkg.json .

# Detect target architecture and set triplet
ARG TARGETARCH
RUN if [ "$TARGETARCH" = "arm64" ]; then \
        echo "arm64-linux" > /tmp/triplet; \
    else \
        echo "x64-linux" > /tmp/triplet; \
    fi

# Pre-install dependencies
RUN TRIPLET=$(cat /tmp/triplet) && \
    vcpkg install --triplet $TRIPLET

# Copy source and build
COPY . .

RUN TRIPLET=$(cat /tmp/triplet) && \
    cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_TARGET_TRIPLET=$TRIPLET \
    && cmake --build build --target cfrp

# Runtime stage — keep Alpine for the small footprint
FROM alpine:latest

RUN apk add --no-cache ca-certificates libstdc++ libgcc

WORKDIR /app

COPY --from=builder /app/build/cfrp .
COPY --from=builder /app/server.toml .
COPY --from=builder /app/client.toml .

RUN addgroup -S cfrp && adduser -S cfrp -G cfrp && \
    chown -R cfrp:cfrp /app

USER cfrp

ENTRYPOINT ["./cfrp"]
CMD ["--help"]
