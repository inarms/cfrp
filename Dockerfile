# Build stage — using Alpine 3.16 for CMake 3.23 (satisfies vcpkg >= 3.21 requirement)
FROM alpine:3.16 AS builder

# Install build dependencies
RUN apk add --no-cache \
    build-base \
    cmake \
    ninja \
    git \
    pkgconf \
    zip \
    unzip \
    curl \
    bash \
    perl \
    linux-headers \
    python3 \
    tar \
    autoconf \
    automake \
    libtool

# Install vcpkg — pinned to builtin-baseline
WORKDIR /opt
COPY vcpkg.json /tmp/vcpkg.json
RUN VCPKG_COMMIT=$(python3 -c "import json; print(json.load(open('/tmp/vcpkg.json'))['builtin-baseline'])") && \
    git clone --depth 1 https://github.com/microsoft/vcpkg.git && \
    git -C vcpkg fetch --depth 1 origin ${VCPKG_COMMIT} && \
    git -C vcpkg checkout ${VCPKG_COMMIT} && \
    ./vcpkg/bootstrap-vcpkg.sh -disableMetrics

ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH="${PATH}:${VCPKG_ROOT}"
ENV VCPKG_FORCE_SYSTEM_BINARIES=1

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

# Build with static linking to ensure compatibility
RUN TRIPLET=$(cat /tmp/triplet) && \
    cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_TARGET_TRIPLET=$TRIPLET \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
    && cmake --build build --target cfrp

# Runtime stage — Latest Alpine for the smallest/most secure footprint
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
