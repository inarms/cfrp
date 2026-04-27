# Build stage — Download and prepare binaries
FROM alpine:latest AS downloader

ARG TARGETARCH
ARG VERSION=v0.2.3

# Install curl to download release artifacts
RUN apk add --no-cache curl

WORKDIR /tmp

# Map Docker architecture to release artifact naming convention
RUN if [ "$TARGETARCH" = "arm64" ]; then \
        ARCH="arm64"; \
    else \
        ARCH="amd64"; \
    fi && \
    curl -L -o server.tar.gz "https://github.com/inarms/cfrp/releases/download/${VERSION}/cfrp-server-linux-${ARCH}.tar.gz" && \
    mkdir -p /app && \
    tar -xzf server.tar.gz -C /app

# Runtime stage
FROM alpine:latest

# Install minimal runtime dependencies
RUN apk add --no-cache ca-certificates libstdc++ libgcc

WORKDIR /app

# Copy the binary and config files from the downloader stage
COPY --from=downloader /app/cfrp .
COPY --from=downloader /app/server.toml .
# Use the local client.toml from context as it's not always in the server pack
COPY client.toml .

# Create a non-root user for security
RUN addgroup -S cfrp && adduser -S cfrp -G cfrp && \
    chown -R cfrp:cfrp /app
USER cfrp

# Application entry point
ENTRYPOINT ["./cfrp"]
CMD ["--help"]
