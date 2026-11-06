#!/bin/sh
# From macOS/Windows (or any Docker host): host/docker/run.sh <command...> runs it in the
# ox-host container with this repository at /src, e.g.
#   host/docker/run.sh host/build.sh build
#   host/docker/run.sh host/docker/shot.sh /src/build-docker/shot.png 8 F9 sleep:1
# The container builds in build-docker/, not build-host/: on a Linux host what it writes is root's, and its CMake
# cache names /src, so a build of this PC's own in build-host/ would fail on both. Empty it (the volume too) with
# host/docker/run.sh host/build.sh clean
set -e
cd "$(dirname "$0")/../.."
docker image inspect ox-host >/dev/null 2>&1 || docker build -t ox-host host/docker
# the stub radio's switches (HOST_STUB_IQ and the others in hw_stub.c) pass through; a file they name has to be
# inside the repository, which the container sees at /src
set -- $(env | sed -n 's/^\(HOST_STUB_[A-Z_]*\)=.*/-e \1/p') -e HOST_BUILD_DIR=/src/build-docker -v "$PWD":/src \
    -v ox-deps:/src/build-docker/deps ox-host "$@"
exec docker run --rm "$@"
