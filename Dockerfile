# Build CrIS-MPC and its in-tree EMP static library on Ubuntu 22.04.
# First run: git submodule update --init --recursive
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        libssl-dev \
        python3 \
        ca-certificates \
        socat \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel 4

CMD ["/bin/bash"]
