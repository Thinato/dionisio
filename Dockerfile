FROM debian:bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake make python3 ca-certificates libcurl4-openssl-dev libsqlite3-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src src
COPY tests tests
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DDIONISIO_TUI=OFF \
    && cmake --build build -j --target dionisio-server

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libcurl4 libsqlite3-0 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/dionisio-server /usr/local/bin/dionisio-server
RUN useradd -r -u 10001 app
USER app
ENV PORT=8080
CMD ["dionisio-server"]
