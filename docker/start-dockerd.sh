#!/bin/bash
# Start dockerd init script for Wolf container
# This runs before Wolf starts (via GOW's /etc/cont-init.d/ system)

set -e

echo "🐳 Starting Wolf's isolated dockerd..."

# Configure dockerd to use NVIDIA runtime (if NVIDIA GPU present)
# This allows sandboxes to use --gpus or --runtime=nvidia
mkdir -p /etc/docker
cat > /etc/docker/daemon.json <<EOF
{
  "runtimes": {
    "nvidia": {
      "path": "nvidia-container-runtime",
      "runtimeArgs": []
    }
  },
  "storage-driver": "overlay2",
  "log-level": "error"
}
EOF

echo "Configured Wolf's dockerd with nvidia runtime support"

# Start dockerd in background
# This is Wolf's OWN dockerd, NOT the host's docker!
# Sandboxes will mount this socket, not the host socket
dockerd --config-file /etc/docker/daemon.json \
    --host=unix:///var/run/docker.sock \
    >/var/log/dockerd.log 2>&1 &

DOCKERD_PID=$!
echo "Started dockerd with PID: $DOCKERD_PID"

# Wait for dockerd to be ready (max 30 seconds)
TIMEOUT=30
ELAPSED=0
until docker info >/dev/null 2>&1; do
    if [ $ELAPSED -ge $TIMEOUT ]; then
        echo "❌ ERROR: Wolf's dockerd failed to start within $TIMEOUT seconds"
        echo "Dockerd logs:"
        tail -50 /var/log/dockerd.log
        exit 1
    fi
    echo "Waiting for Wolf's dockerd to be ready... ($ELAPSED/$TIMEOUT)"
    sleep 1
    ELAPSED=$((ELAPSED + 1))
done

echo "✅ Wolf's dockerd is ready!"
docker info 2>&1 | head -5

# dockerd continues running in background
# Wolf will create sandbox containers in this dockerd
