FROM devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282

# Host tools also support exercising the isolated engine on Linux before
# transferring a cross-compiled application to a physical Switch.
RUN apt-get update && apt-get install -y --no-install-recommends \
    meson ninja-build python3 pkg-config libicu-dev \
    libcurl4-openssl-dev libjansson-dev zlib1g-dev liblzma-dev \
    libzstd-dev libbz2-dev libsdl2-dev libsdl2-image-dev \
    libsdl2-ttf-dev libsdl2-mixer-dev \
    && rm -rf /var/lib/apt/lists/*
