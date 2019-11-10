ARG BASE_VERSION
ARG DOCKER_HUB_NAME

FROM $DOCKER_HUB_NAME:$BASE_VERSION

WORKDIR /libdragon

ARG LIBDRAGON_VERSION_MAJOR
ARG LIBDRAGON_VERSION_MINOR
ARG LIBDRAGON_VERSION_REVISION
ARG LIBDRAGON_COMMIT_SHA
ARG MIKMOD_COMMIT_SHA=738b1e8b11b470360b1b919680d1d88429d9d174

# Build the actual library here & build and install mikmod
RUN git clone https://github.com/DragonMinded/libdragon.git/ ./libdragon-code && \
    cd ./libdragon-code && \
    git checkout $LIBDRAGON_COMMIT_SHA && \
    make --jobs 8 clean && \
    make --jobs 8 && \
    make install && \
    make --jobs 8 tools && \
    make tools-install && \
    rm -rf * && \
    cd .. * && \
    git clone https://github.com/networkfusion/libmikmod.git /tmp/libmikmod && \
    cd /tmp/libmikmod/n64 && \
    git checkout $MIKMOD_COMMIT_SHA && \
    make -j8 && \
    make install && \
    cd /libdragon && \
    rm -rf /tmp/libmikmod