# The GUI on a PC, in a container: for macOS and Windows (Docker Desktop), or a Linux without
# the development packages. host/docker/run.sh builds this image and runs host/build.sh in it,
# with the repository mounted; host/docker/shot.sh runs the GUI on a virtual X display (Xvfb) and saves
# a screenshot.
FROM debian:trixie
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config git curl ca-certificates bzip2 \
      autoconf automake libtool \
      sqlite3 libsqlite3-dev libsdl2-dev libdrm-dev libxkbcommon-dev libnm-dev libdbus-glib-1-dev \
      libpng-dev libjpeg-dev libsndfile1-dev libudev-dev libpulse-dev libasound2-dev libgps-dev libfftw3-dev \
      pulseaudio dbus xvfb x11-utils imagemagick xdotool \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
