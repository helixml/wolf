# syntax=docker/dockerfile:1.4
ARG BASE_IMAGE=ghcr.io/games-on-whales/gstreamer:1.26.7
########################################################
FROM $BASE_IMAGE AS wolf-builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -y && \
    apt-get install -y --no-install-recommends \
    curl \
    ca-certificates \
    ninja-build \
    cmake \
    pkg-config \
    ccache \
    git \
    clang \
    build-essential \
    libboost-thread-dev libboost-locale-dev libboost-filesystem-dev libboost-log-dev libboost-stacktrace-dev libboost-container-dev \
    libwayland-dev libwayland-server0 libinput-dev libxkbcommon-dev libgbm-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    libevdev-dev \
    libpulse-dev \
    libunwind-dev \
    libudev-dev \
    libdrm-dev \
    libpci-dev \
    libglib2.0-dev libegl-dev libgles-dev libopengl-dev \
    && rm -rf /var/lib/apt/lists/*

## Install Rust in order to build our custom compositor
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
ENV PATH="$HOME/.cargo/bin:${PATH}"

ARG RUST_VERSION=1.89.0
ENV RUST_VERSION=$RUST_VERSION
RUN rustup install $RUST_VERSION && rustup default $RUST_VERSION

WORKDIR /tmp/
RUN <<_GST_WAYLAND_DISPLAY
    #!/bin/bash
    set -e

    git clone https://github.com/games-on-whales/gst-wayland-display
    cd gst-wayland-display
    git checkout e89d9f5d
    cargo install cargo-c
    cargo cinstall -p gst-plugin-wayland-display --prefix=/usr/local/lib/x86_64-linux-gnu/ --libdir=/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0
_GST_WAYLAND_DISPLAY

COPY . /wolf/
WORKDIR /wolf

ENV CCACHE_DIR=/cache/ccache
ENV CMAKE_BUILD_DIR=/cache/cmake-build
ARG BUILD_JOBS=8
# DEBUG BUILD (current) - Full debug symbols for deadlock investigation
# WOLF_CUSTOM_INPUTTINO_SRC uses our vendored inputtino with bugfix for RHEL keyboard issues
RUN --mount=type=cache,target=/cache/ccache \
    cmake -B$CMAKE_BUILD_DIR \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_STANDARD=17 \
    -DCMAKE_CXX_EXTENSIONS=OFF \
    -DCMAKE_CXX_FLAGS="-g3 -O0 -fno-omit-frame-pointer -Wno-missing-template-arg-list-after-template-kw" \
    -DCMAKE_C_FLAGS="-g3 -O0 -fno-omit-frame-pointer" \
    -DBUILD_SHARED_LIBS=OFF \
    -DBoost_USE_STATIC_LIBS=ON \
    -DBUILD_FAKE_UDEV_CLI=ON \
    -DBUILD_TESTING=OFF \
    -DWOLF_CUSTOM_INPUTTINO_SRC=/wolf/third_party/inputtino \
    -G Ninja && \
    ninja -j $BUILD_JOBS -C $CMAKE_BUILD_DIR wolf && \
    ninja -j $BUILD_JOBS -C $CMAKE_BUILD_DIR fake-udev && \
    # We have to copy out the built executables because this will only be available inside the buildkit cache
    cp $CMAKE_BUILD_DIR/src/moonlight-server/wolf /wolf/wolf && \
    cp $CMAKE_BUILD_DIR/src/fake-udev/fake-udev /wolf/fake-udev

# RELEASE BUILD (commented out) - Use for production when debugging complete
# RUN --mount=type=cache,target=/cache/ccache \
#     cmake -B$CMAKE_BUILD_DIR \
#     -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#     -DCMAKE_CXX_STANDARD=17 \
#     -DCMAKE_CXX_EXTENSIONS=OFF \
#     -DCMAKE_CXX_FLAGS="-Wno-missing-template-arg-list-after-template-kw" \
#     -DBUILD_SHARED_LIBS=OFF \
#     -DBoost_USE_STATIC_LIBS=ON \
#     -DBUILD_FAKE_UDEV_CLI=ON \
#     -DBUILD_TESTING=OFF \
#     -G Ninja && \
#     ninja -j $BUILD_JOBS -C $CMAKE_BUILD_DIR wolf && \
#     ninja -j $BUILD_JOBS -C $CMAKE_BUILD_DIR fake-udev && \
#     cp $CMAKE_BUILD_DIR/src/moonlight-server/wolf /wolf/wolf && \
#     cp $CMAKE_BUILD_DIR/src/fake-udev/fake-udev /wolf/fake-udev

########################################################
FROM $BASE_IMAGE AS runner
ENV DEBIAN_FRONTEND=noninteractive

# Wolf runtime dependencies
RUN apt-get update -y && \
    apt-get install -y --no-install-recommends \
    ca-certificates \
    libssl3 \
    libicu76 \
    libevdev2 \
    libudev1 \
    libcurl4 \
    libdrm2 \
    libpci3 \
    libunwind8 \
    && rm -rf /var/lib/apt/lists/*

# Debug tools for deadlock investigation and core dump analysis
RUN apt-get update -y && \
    apt-get install -y --no-install-recommends \
    gdb \
    strace \
    ltrace \
    lsof \
    procps \
    htop \
    binutils \
    && rm -rf /var/lib/apt/lists/*

# Add ddebs repository for debug symbols (including updates)
# Download GPG key from official ddebs.ubuntu.com
# Note: ddebs has plucky and plucky-updates, but NO plucky-security suite
RUN apt-get update -y && \
    apt-get install -y --no-install-recommends wget ca-certificates gnupg && \
    wget -O- http://ddebs.ubuntu.com/dbgsym-release-key.asc | gpg --dearmor -o /usr/share/keyrings/ddebs-archive-keyring.gpg && \
    echo "deb [signed-by=/usr/share/keyrings/ddebs-archive-keyring.gpg] http://ddebs.ubuntu.com plucky main restricted universe multiverse" > /etc/apt/sources.list.d/ddebs.list && \
    echo "deb [signed-by=/usr/share/keyrings/ddebs-archive-keyring.gpg] http://ddebs.ubuntu.com plucky-updates main restricted universe multiverse" >> /etc/apt/sources.list.d/ddebs.list && \
    rm -rf /var/lib/apt/lists/*

# Install debug symbols for security-patched versions
# Wolf binary compiled with -g3 -O0 -fno-omit-frame-pointer (full debug symbols)
# System symbols: pthread_mutex_lock, g_object_set, gst_element_factory_make, futex, epoll
RUN apt-get update -y && \
    apt-get install -y --no-install-recommends \
    libc6-dbg \
    libglib2.0-0t64-dbgsym \
    libgstreamer1.0-0-dbgsym \
    gstreamer1.0-plugins-base-dbgsym \
    gstreamer1.0-plugins-good-dbgsym \
    && rm -rf /var/lib/apt/lists/*

# gst-plugin-wayland runtime dependencies
RUN apt-get update -y && \
    apt-get install -y --no-install-recommends \
    libwayland-server0 libinput10 libxkbcommon0 libgbm1 \
    libglvnd0 libgl1 libglx0 libegl1 libgles2 xwayland hwdata \
    && rm -rf /var/lib/apt/lists/*

# Install Docker inside Wolf container for nested sandboxes
# This allows Wolf to create sandbox containers with its own dockerd (no host docker socket needed!)
RUN apt-get update -y && \
    apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    gnupg \
    lsb-release \
    && mkdir -p /etc/apt/keyrings \
    && curl -fsSL https://download.docker.com/linux/ubuntu/gpg | gpg --dearmor -o /etc/apt/keyrings/docker.gpg \
    && echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" | tee /etc/apt/sources.list.d/docker.list > /dev/null \
    && apt-get update -y \
    && apt-get install -y --no-install-recommends \
    docker-ce-cli \
    docker-ce \
    containerd.io \
    && rm -rf /var/lib/apt/lists/*

# Install NVIDIA Container Toolkit inside Wolf's dockerd
# This enables sandboxes to use --gpus or --runtime=nvidia for GPU-accelerated apps
# CRITICAL: Use v1.18.0+ (CVE-2025-23266 fixed in 1.18.0, CVSS 9.0 container escape)
RUN curl -fsSL https://nvidia.github.io/libnvidia-container/gpgkey | gpg --dearmor -o /usr/share/keyrings/nvidia-container-toolkit-keyring.gpg \
    && curl -s -L https://nvidia.github.io/libnvidia-container/stable/deb/nvidia-container-toolkit.list | \
        sed 's#deb https://#deb [signed-by=/usr/share/keyrings/nvidia-container-toolkit-keyring.gpg] https://#g' | \
        tee /etc/apt/sources.list.d/nvidia-container-toolkit.list \
    && apt-get update -y \
    && apt-get install -y --no-install-recommends \
    nvidia-container-toolkit \
    nvidia-container-runtime \
    && rm -rf /var/lib/apt/lists/*

ENV GST_PLUGIN_PATH=/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0/
# Copying out our custom compositor from the build stage
COPY --from=wolf-builder /usr/local/lib/x86_64-linux-gnu/gstreamer-1.0/* $GST_PLUGIN_PATH
COPY --from=wolf-builder /usr/local/lib/liblibgstwaylanddisplay* /usr/local/lib/

WORKDIR /wolf

ENV WOLF_CFG_FOLDER=/etc/wolf/cfg

COPY --from=wolf-builder /wolf/wolf /wolf/wolf
COPY --from=wolf-builder /wolf/fake-udev /wolf/fake-udev

# Add Helix init script and config template for Wolf initialization
# Template goes to /opt/wolf-defaults (NOT bind-mounted, always available from image)
RUN mkdir -p /opt/wolf-defaults
COPY docker/config.toml.template /opt/wolf-defaults/config.toml.template
COPY docker/init-wolf-config.sh /etc/cont-init.d/05-init-wolf-config.sh
RUN chmod +x /etc/cont-init.d/05-init-wolf-config.sh

# Add dockerd startup script (runs before Wolf via cont-init.d system)
COPY docker/start-dockerd.sh /etc/cont-init.d/04-start-dockerd.sh
RUN chmod +x /etc/cont-init.d/04-start-dockerd.sh

ENV GST_GL_API=gles2 \
    GST_GL_PLATFORM=egl \
    GST_GL_WINDOW=surfaceless \
    WOLF_USE_ZERO_COPY=TRUE \
    WOLF_LOG_LEVEL=INFO \
    WOLF_CFG_FILE=$WOLF_CFG_FOLDER/config.toml \
    WOLF_PRIVATE_KEY_FILE=$WOLF_CFG_FOLDER/key.pem \
    WOLF_PRIVATE_CERT_FILE=$WOLF_CFG_FOLDER/cert.pem \
    WOLF_PULSE_IMAGE=ghcr.io/games-on-whales/pulseaudio:master \
    WOLF_RENDER_NODE=/dev/dri/renderD128 \
    WOLF_STOP_CONTAINER_ON_EXIT=TRUE \
    WOLF_DOCKER_SOCKET=/var/run/docker.sock \
    RUST_BACKTRACE=full \
    RUST_LOG=WARN \
    HOST_APPS_STATE_FOLDER=/etc/wolf \
    GST_DEBUG=2 \
    PUID=0 \
    PGID=0 \
    UNAME="root"

# Setting up XDG_RUNTIME_DIR this will automatically create a volume when starting the container
VOLUME /run/user/wolf/
ENV XDG_RUNTIME_DIR=/run/user/wolf

# HTTPS
EXPOSE 47984/tcp
# HTTP
EXPOSE 47989/tcp
# Control
EXPOSE 47999/udp
# RTSP
EXPOSE 48010/tcp
# Video
EXPOSE 48100/udp
# Audio
EXPOSE 48200/udp

LABEL org.opencontainers.image.source="https://github.com/games-on-whales/wolf/"
LABEL org.opencontainers.image.description="Wolf: stream virtual desktops and games in Docker"

# See GOW/base-app
COPY --chmod=777 docker/startup.sh /opt/gow/startup-app.sh
ENTRYPOINT ["/entrypoint.sh"]
