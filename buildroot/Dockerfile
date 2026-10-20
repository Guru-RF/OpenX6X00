# The build host of the CI workflow: Ubuntu 22.04 with buildroot's requirements.
# Build it once:  docker build -t openx6x00-build buildroot
# then see local-build.sh.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential bc bison bzip2 ca-certificates ccache cmake cpio curl file flex gawk git gperf \
    gzip libncurses5-dev libssl-dev make patch perl python3 python3-dev python3-setuptools rsync \
    sqlite3 texinfo unzip wget xz-utils zip zlib1g-dev locales \
    && rm -rf /var/lib/apt/lists/* \
    && locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
# The GUI and the X6100 control library need CMake 3.23 or newer; Ubuntu 22.04 ships 3.22
ARG CMAKE_VER=3.28.3
RUN wget -q https://github.com/Kitware/CMake/releases/download/v${CMAKE_VER}/cmake-${CMAKE_VER}-linux-x86_64.sh -O /tmp/cmake.sh \
    && sh /tmp/cmake.sh --skip-license --prefix=/usr/local && rm /tmp/cmake.sh
