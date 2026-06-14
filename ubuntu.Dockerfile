# syntax=docker/dockerfile:1
FROM ubuntu:jammy AS build

ENV TZ=Europe/London

WORKDIR /build

RUN apt-get update && apt-get install -y --no-install-recommends \
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

RUN apt-get update && apt-get install -y --no-install-recommends \
    liblmdb0 \
    libsecp256k1-0 \
    libzstd1 \
    libflatbuffers1 \
    libb2-1 \
    && rm -rf /var/lib/apt/lists/*

# Create a non-root user for security
RUN useradd -m -d /app -s /bin/bash strfry && \
    chown -R strfry:strfry /app

# Switch to the unprivileged user
USER strfry

COPY --from=build --chown=strfry:strfry /build/strfry /app/strfry
COPY --from=build --chown=strfry:strfry /build/strfry.conf /app/strfry.conf
COPY --from=build --chown=strfry:strfry /build/strfry-db /app/strfry-db

EXPOSE 7777

ENTRYPOINT ["/app/strfry"]
CMD ["relay"]
