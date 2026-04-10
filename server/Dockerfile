FROM alpine:latest AS builder

# Install build dependencies
RUN apk add --no-cache gcc musl-dev make openssl-dev sqlite-dev

# Set working directory
WORKDIR /app

# Copy source files
COPY server.h server.c websocket.c db.c cJSON.h cJSON.c Makefile ./

# Build the server
RUN make

# Multi-stage build to reduce image size
FROM alpine:latest

# Install runtime dependencies required by dynamic linking
RUN apk add --no-cache openssl sqlite-libs

WORKDIR /app

# Copy the compiled binary from the builder stage
COPY --from=builder /app/collab_server .

# Expose the WebSocket port
EXPOSE 8080

# Keep the DB in a volume for persistence across container restarts
VOLUME ["/app"]

# Run the server
CMD ["./collab_server"]
