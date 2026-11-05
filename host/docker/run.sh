#!/bin/sh
# From macOS/Windows (or any Docker host): host/docker/run.sh <command...> runs it in the
# ox-host container with this repository at /src, e.g.
#   host/docker/run.sh host/build.sh build
#   host/docker/run.sh host/docker/shot.sh /src/build-host/shot.png 8 F9 sleep:1
set -e
cd "$(dirname "$0")/../.."
docker image inspect ox-host >/dev/null 2>&1 || docker build -t ox-host host/docker
# the stub radio's switches (HOST_STUB_IQ and the others in hw_stub.c) pass through; a file they name has to be
# inside the repository, which the container sees at /src
set -- $(env | sed -n 's/^\(HOST_STUB_[A-Z_]*\)=.*/-e \1/p') -v "$PWD":/src -v ox-deps:/src/build-host/deps ox-host "$@"
exec docker run --rm "$@"
