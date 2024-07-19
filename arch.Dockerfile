# arch based Dockerfile built by mleku
# npub1mlekuhhxqq6p8w9x5fs469cjk3fu9zw7uptujr55zmpuhhj48u3qnwx3q5
# this is a basic dockerfile showing the build on arch linux

FROM archlinux:latest

WORKDIR /app

COPY . .

# update pacman keys and sync repos
RUN pacman -Syu --noconfirm

RUN pacman -S --noconfirm \
# build essentials
  base-devel git \
# flatbuffers, lmdb, libsecp256k1
  flatbuffers lmdb libsecp256k1

# update submodules
RUN git submodule update --init 

# build golpe
RUN make setup-golpe 

# build strfry
RUN make

EXPOSE 7777

ENTRYPOINT ["/app/strfry"]
CMD ["relay"]
