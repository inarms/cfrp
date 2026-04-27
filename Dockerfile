# Build stage — runs natively on each target platform via QEMU
FROM alpine:latest AS builder

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

# Install vcpkg
WORKDIR /opt
RUN git clone --depth 1 https://github.com/microsoft/vcpkg.git && \
    ./vcpkg/bootstrap-vcpkg.sh -disableMetrics

ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH="${PATH}:${VCPKG_ROOT}"
ENV VCPKG_FORCE_SYSTEM_BINARIES=1

WORKDIR /app
COPY vcpkg.json .

# Detect architecture and set triplet from the running platform (not build platform)
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

# Runtime stage
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
