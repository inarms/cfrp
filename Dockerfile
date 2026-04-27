# Runtime stage
FROM alpine:latest

ARG TARGETARCH

# Install minimal runtime dependencies
RUN apk add --no-cache ca-certificates libstdc++ libgcc

WORKDIR /app

# Copy the binary and config files from the context (prepared by CI)
# Binaries are expected at bin/${TARGETARCH}/
COPY bin/${TARGETARCH}/cfrp .
COPY bin/${TARGETARCH}/server.toml .
COPY client.toml .

# Create a non-root user for security
RUN addgroup -S cfrp && adduser -S cfrp -G cfrp && \
    chown -R cfrp:cfrp /app
USER cfrp

# Application entry point
ENTRYPOINT ["cfrp"]
CMD ["--help"]
