# syntax=docker/dockerfile:1
# Runtime stage
FROM alpine:latest AS builder

 # Install build dependencies required by Alpine, CMake, and vcpkg
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

# --- The Alpine Samurai Patch ---
# Alpine's 'ninja' is actually a POSIX-strict clone called 'samurai'.
# It crashes when vcpkg runs `ninja install -v` (it expects `ninja -v install`).
# We replace the symlink with a Python wrapper that intercepts and re-orders the arguments.
RUN rm /usr/bin/ninja && \
    echo '#!/usr/bin/env python3' > /usr/bin/ninja && \
    echo 'import sys, os' >> /usr/bin/ninja && \
    echo 'args = sys.argv[1:]' >> /usr/bin/ninja && \
    echo 'if "-v" in args:' >> /usr/bin/ninja && \
    echo '    args.remove("-v")' >> /usr/bin/ninja && \
    echo '    args.insert(0, "-v")' >> /usr/bin/ninja && \
    echo 'os.execv("/usr/bin/samu", ["samu"] + args)' >> /usr/bin/ninja && \
    chmod +x /usr/bin/ninja

ENV VCPKG_FORCE_SYSTEM_BINARIES=1
ENV VCPKG_ROOT=/vcpkg
ENV VCPKG_DEFAULT_BINARY_CACHE=/root/.cache/vcpkg

# Use ccache for faster incremental builds
ENV CMAKE_CXX_COMPILER_LAUNCHER=ccache
ENV CMAKE_C_COMPILER_LAUNCHER=ccache
ENV CCACHE_DIR=/root/.cache/ccache

# Increase concurrency - remove hard limits to use available CPU power
# Or set to a reasonable default like 4 if you want to limit it
ENV VCPKG_MAX_CONCURRENCY=
ENV CMAKE_BUILD_PARALLEL_LEVEL=

# Clone vcpkg (use partial clone to save space/time while allowing access to baseline commits)
RUN git clone --filter=blob:none https://github.com/microsoft/vcpkg.git /vcpkg && \
   /vcpkg/bootstrap-vcpkg.sh -disableMetrics

# Inject the flag into vcpkg's Linux configurations to prevent warnings from crashing the build
RUN find /vcpkg/triplets -name "*-linux.cmake" -exec sh -c 'echo "set(VCPKG_C_FLAGS \"\${VCPKG_C_FLAGS} -Wno-error=stringop-overflow\")" >> "{}"' \; && \
    find /vcpkg/triplets -name "*-linux.cmake" -exec sh -c 'echo "set(VCPKG_CXX_FLAGS \"\${VCPKG_CXX_FLAGS} -Wno-error=stringop-overflow\")" >> "{}"' \;

# Install minimal runtime dependencies
RUN apk add --no-cache ca-certificates libstdc++ libgcc

WORKDIR /src

# NEW: Cache dependencies by copying only vcpkg.json first
COPY vcpkg.json .
ARG TARGETARCH
# Use a cache mount for vcpkg downloads and artifacts
RUN --mount=type=cache,target=/root/.cache/vcpkg \
    if [ "$TARGETARCH" = "arm64" ]; then TRIPLET=arm64-linux; else TRIPLET=x64-linux; fi && \
    /vcpkg/vcpkg install --triplet $TRIPLET --x-manifest-root=.

# Copy the rest of the project source code
COPY . .

 # Build the project using CMake and vcpkg toolchain
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

FROM alpine:latest
# Install minimal runtime dependencies
RUN apk add --no-cache ca-certificates libstdc++ libgcc

WORKDIR /app
COPY --from=builder /src/build/cfrp /app/
COPY --from=builder /src/server.toml /app/
COPY --from=builder /src/client.toml /app/

# Create a non-root user for security
RUN addgroup -S cfrp && adduser -S cfrp -G cfrp && \
    chown -R cfrp:cfrp /app

USER cfrp
# Application entry point
ENTRYPOINT ["/app/cfrp"]
CMD ["--help"]
