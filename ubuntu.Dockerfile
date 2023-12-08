FROM ubuntu:jammy as build
ENV TZ=Europe/London
WORKDIR /build
RUN apt update && apt install -y --no-install-recommends \
    git g++ make pkg-config libtool ca-certificates \
    libyaml-perl libtemplate-perl libregexp-grammars-perl libssl-dev zlib1g-dev \
    liblmdb-dev libflatbuffers-dev libsecp256k1-dev \
    libzstd-dev

COPY . .
RUN make clean
RUN git submodule update --init
RUN make setup-golpe
RUN make -j4

FROM ubuntu:jammy as runner
WORKDIR /app

RUN apt update && apt install -y --no-install-recommends \
    liblmdb0 libflatbuffers1 libsecp256k1-0 libb2-1 libzstd1 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /build/strfry strfry
ENTRYPOINT ["/app/strfry"]
CMD ["relay"]
