FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    g++-11 \
    libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . /app

RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_CXX_COMPILER=g++-11 -DCMAKE_BUILD_TYPE=Release && \
    make -j"$(nproc)"

# Fail the image build if the test suite does not pass.
RUN cd build && ctest --output-on-failure

# Runtime image: no compiler, just the binary.
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /app/build/flashkv /app/flashkv

EXPOSE 6379

CMD ["/app/flashkv", "--port", "6379"]
