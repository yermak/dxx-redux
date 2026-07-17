# d1x-redux dedicated game server.
# Game data (descent.hog/descent.pig, add-on missions) is NOT included --
# mount it at /data.
FROM debian:bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build libsdl1.2-dev libphysfs-dev \
    && rm -rf /var/lib/apt/lists/*
COPY . /src/d1
WORKDIR /src/d1
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DOPENGL=OFF -DOPENGLMERGE=OFF -DSDLMIXER=OFF -DPNG=OFF -DTRACKER=OFF \
    && cmake --build build -j

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsdl1.2debian libphysfs1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/d1/build/main/d1x-redux /usr/local/bin/d1x-redux
COPY --from=build /src/d1/build/gameserver/d1x-gameserver /usr/local/bin/d1x-gameserver
ENV GS_ENGINE=/usr/local/bin/d1x-redux \
    GS_HOGDIR=/data
VOLUME /data
EXPOSE 42424/udp 42425-42440/udp
ENTRYPOINT ["/usr/local/bin/d1x-gameserver"]
