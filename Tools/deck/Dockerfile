# Build image for a portable Linux x86_64 SDL build (Steam Deck / SteamOS 3).
# Ubuntu 22.04 = glibc 2.35, old enough to load on any current SteamOS.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates pkg-config patchelf file \
        libsdl2-dev libsdl2-ttf-dev libfontconfig1-dev \
        libgl1-mesa-dev libglu1-mesa-dev libegl1-mesa-dev \
        libx11-dev libwayland-dev libxkbcommon-dev \
        zlib1g-dev ccache \
    && rm -rf /var/lib/apt/lists/*
