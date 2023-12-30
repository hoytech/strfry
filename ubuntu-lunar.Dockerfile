FROM ubuntu:lunar as build
ENV TZ=Europe/London
WORKDIR /build
RUN apt update && apt install -y --no-install-recommends \
    git g++ make pkg-config libtool ca-certificates \
    libyaml-perl libtemplate-perl libregexp-grammars-perl libssl-dev zlib1g-dev \
    liblmdb-dev libflatbuffers-dev libsecp256k1-dev libzstd-dev \
    debhelper devscripts build-essential

COPY . .
RUN git submodule update --init
RUN make setup-golpe
RUN make clean
RUN make -j4
RUN dpkg-buildpackage --build=binary -us -uc
RUN mv ../*.deb .
ARG FN="`ls *.deb`"
RUN echo "${FN}" > /build/pkgfilename

FROM ubuntu:lunar as runner
WORKDIR /app

RUN apt update && apt install -y --no-install-recommends \
    liblmdb0 libflatbuffers2 libsecp256k1-1 libb2-1 libzstd1 libssl3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /build/strfry strfry
COPY --from=build /build/pkgfilename /app/
RUN FN=$(cat /app/pkgfilename) && echo "Filename is: ${FN}"
COPY --from=build /build/${FN} ${FN}


ENTRYPOINT ["/app/strfry"]
CMD ["relay"]
