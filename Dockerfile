# Built by Akito
# npub1wprtv89px7z2ut04vvquscpmyfuzvcxttwy2csvla5lvwyj807qqz5aqle

FROM alpine:3.18.3 AS build

ENV TZ=Europe/London

WORKDIR /build

RUN \
  apk --no-cache add \
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

COPY . .

RUN git submodule update --init

RUN make setup-golpe

RUN --mount=type=cache,target=/build/.cache \
    make -j4

FROM alpine:3.18.3

WORKDIR /app

RUN \
  apk --no-cache add \
    lmdb \
    flatbuffers \
    libsecp256k1 \
    libb2 \
    zstd \
    libressl \
  && rm -rf /var/cache/apk/*

COPY --from=build /build/strfry strfry
COPY --from=build /build/strfry.conf strfry.conf
COPY --from=build /build/strfry-db strfry-db

EXPOSE 7777

ENTRYPOINT ["/app/strfry"]
CMD ["relay"]
