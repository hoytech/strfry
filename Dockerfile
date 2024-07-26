# Runtime dependencies
FROM alpine:3.18.3 AS runtime-deps
RUN apk --no-cache update && apk --no-cache add \
    lmdb \
    flatbuffers \
    libsecp256k1 \
    libb2 \
    zstd \
    libressl

# Build dependencies
FROM runtime-deps AS build-deps
RUN apk update && apk add \
    linux-headers \
    git \
    g++ \
    make \
    perl \
    pkgconfig \
    libtool \
    ca-certificates \
    libressl-dev \
    zlib-dev \
    lmdb-dev \
    flatbuffers-dev \
    libsecp256k1-dev \
    zstd-dev

# Stage 1: Build the application using the build-deps base image
FROM build-deps AS build

WORKDIR /build

COPY . .

RUN make clean \
 && git submodule update --init \
 && make setup-golpe \
 && make -j4

# Stage 2: Create the final runtime image using the runtime-deps base image
FROM runtime-deps AS runtime

WORKDIR /app

COPY --from=build /build/strfry strfry

EXPOSE 7777

ENTRYPOINT ["/app/strfry"]
CMD ["relay"]
