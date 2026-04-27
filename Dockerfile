# Build stage
FROM --platform=$BUILDPLATFORM alpine:latest AS builder

# Install build dependencies
# tar, curl, zip, unzip are required by vcpkg
# perl, python3, linux-headers are required by some dependencies (like wolfssl/ngtcp2)
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
    tar

# Install vcpkg
WORKDIR /opt
RUN git clone --depth 1 https://github.com/microsoft/vcpkg.git && \
    ./vcpkg/bootstrap-vcpkg.sh -disableMetrics

ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH="${PATH}:${VCPKG_ROOT}"
# Force vcpkg to use system binaries on Alpine (musl)
ENV VCPKG_FORCE_SYSTEM_BINARIES=1

# Set workdir
WORKDIR /app

# Copy dependency manifest
COPY vcpkg.json .

# Build-time architecture detection
ARG TARGETPLATFORM
RUN if [ "$TARGETPLATFORM" = "linux/arm64" ]; then \
        echo "arm64-linux" > /tmp/triplet; \
    elif [ "$TARGETPLATFORM" = "linux/amd64" ]; then \
        echo "x64-linux" > /tmp/triplet; \
    else \
        echo "x64-linux" > /tmp/triplet; \
    fi

# Pre-install dependencies (improves caching)
RUN TRIPLET=$(cat /tmp/triplet) && \
    vcpkg install --triplet $TRIPLET

# Copy source code
COPY . .

# Build the application
RUN TRIPLET=$(cat /tmp/triplet) && \
    cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=$TRIPLET \
    && cmake --build build

# Runtime stage
# Using Alpine for the smallest footprint (approx 5MB) while maintaining usability
FROM alpine:latest

# Install minimal runtime dependencies
# ca-certificates for SSL/TLS, libstdc++ and libgcc for C++ runtime
RUN apk add --no-cache ca-certificates libstdc++ libgcc

WORKDIR /app

# Copy the binary and config files
COPY --from=builder /app/build/cfrp .
COPY --from=builder /app/server.toml .
COPY --from=builder /app/client.toml .

# Create a non-root user for security
RUN addgroup -S cfrp && adduser -S cfrp -G cfrp && \
    chown -R cfrp:cfrp /app
USER cfrp

# Application entry point
ENTRYPOINT ["./cfrp"]
CMD ["--help"]
