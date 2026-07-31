# syntax=docker/dockerfile:1
FROM alpine:3.22 AS builder

# Install build dependencies
# Alpine 3.22 provides native Ninja 1.12+ in the main repositories
RUN --mount=type=cache,target=/var/cache/apk \
    apk add --no-cache \
    build-base \
    cmake \
    ninja \
    git \
    curl \
    zip \
    unzip \
    tar \
    pkgconf \
    linux-headers \
    python3 \
    ccache

# Force vcpkg to ONLY use system-installed binaries (Ninja, CMake, etc.)
# and never download pre-built glibc executables
ENV VCPKG_FORCE_SYSTEM_BINARIES=1
ENV VCPKG_ROOT=/vcpkg
ENV VCPKG_DEFAULT_BINARY_CACHE=/root/.cache/vcpkg

# Use ccache for faster incremental builds
ENV CMAKE_CXX_COMPILER_LAUNCHER=ccache
ENV CMAKE_C_COMPILER_LAUNCHER=ccache
ENV CCACHE_DIR=/root/.cache/ccache

# Clone vcpkg
RUN git clone --filter=blob:none https://github.com/microsoft/vcpkg.git /vcpkg && \
    git -C /vcpkg checkout 0ca64b4e1c70fa6d9f53b369b8f3f0843797c20c && \
    /vcpkg/bootstrap-vcpkg.sh -disableMetrics

# Inject warning suppressions into triplets
RUN find /vcpkg/triplets -name "*-linux.cmake" -exec sh -c 'echo "set(VCPKG_C_FLAGS \"\${VCPKG_C_FLAGS} -Wno-error=stringop-overflow\")" >> "{}"' \; && \
    find /vcpkg/triplets -name "*-linux.cmake" -exec sh -c 'echo "set(VCPKG_CXX_FLAGS \"\${VCPKG_CXX_FLAGS} -Wno-error=stringop-overflow\")" >> "{}"' \;

WORKDIR /src

# Cache dependencies by copying vcpkg.json first
COPY vcpkg.json .
ARG TARGETARCH

# Disable downloads of external tools inside vcpkg
RUN --mount=type=cache,target=/root/.cache/vcpkg \
    if [ "$TARGETARCH" = "arm64" ]; then TRIPLET=arm64-linux; else TRIPLET=x64-linux; fi && \
    /vcpkg/vcpkg install --triplet $TRIPLET --x-manifest-root=.

# Copy source code
COPY . .

# Build project
RUN --mount=type=cache,target=/root/.cache/vcpkg \
    --mount=type=cache,target=/root/.cache/ccache \
    if [ "$TARGETARCH" = "arm64" ]; then TRIPLET=arm64-linux; else TRIPLET=x64-linux; fi && \
    cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=$TRIPLET \
    -G Ninja

RUN --mount=type=cache,target=/root/.cache/ccache \
    cmake --build build

# Final runtime stage
FROM alpine:3.22
RUN apk add --no-cache ca-certificates libstdc++ libgcc

WORKDIR /app
COPY --from=builder /src/build/cfrp /app/
COPY --from=builder /src/server.toml /app/
COPY --from=builder /src/client.toml /app/

RUN addgroup -S cfrp && adduser -S cfrp -G cfrp && \
    chown -R cfrp:cfrp /app

USER cfrp
ENTRYPOINT ["/app/cfrp"]
CMD ["--help"]
