FROM ubuntu:jammy AS build

ENV TZ=Europe/London

WORKDIR /build

RUN apt update && apt install -y --no-install-recommends \
    git g++ make pkg-config libtool ca-certificates \
    libssl-dev zlib1g-dev liblmdb-dev libflatbuffers-dev \
    libsecp256k1-dev libzstd-dev

COPY . .

RUN git submodule update --init

RUN make setup-golpe

RUN --mount=type=cache,target=/build/.cache \
    make -j4

FROM ubuntu:jammy AS runner

WORKDIR /app

RUN apt update && apt install -y --no-install-recommends \
    liblmdb0 libflatbuffers1 libsecp256k1-0 libb2-1 libzstd1 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /build/strfry strfry
COPY --from=build /build/strfry.conf strfry.conf
COPY --from=build /build/strfry-db strfry-db

EXPOSE 7777

ENTRYPOINT ["/app/strfry"]
CMD ["relay"]
