# syntax=docker/dockerfile:1
FROM alpine:3.22

# Install gcompat and C++ runtimes so Ubuntu-compiled glibc binaries run on musl
RUN apk add --no-cache ca-certificates libstdc++ libgcc gcompat

WORKDIR /app

# Docker Buildx automatically sets TARGETARCH (amd64, arm64)
ARG TARGETARCH

# Copy the pre-built binary compiled by GitHub Actions from the prepared folder
COPY release-artifacts/cfrp-server-linux-${TARGETARCH}/cfrp /app/cfrp
COPY server.toml /app/
COPY client.toml /app/

RUN chmod +x /app/cfrp && \
    addgroup -S cfrp && adduser -S cfrp -G cfrp && \
    chown -R cfrp:cfrp /app

USER cfrp
ENTRYPOINT ["/app/cfrp"]
CMD ["--help"]
