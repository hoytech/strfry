# arch based Dockerfile built by mleku
# npub1mlekuhhxqq6p8w9x5fs469cjk3fu9zw7uptujr55zmpuhhj48u3qnwx3q5

FROM archlinux:latest AS build

WORKDIR /build

COPY . .

RUN pacman -Syu --noconfirm
RUN pacman -S --noconfirm base-devel git

# perl things
RUN pacman -S --noconfirm cpanminus
RUN pacman -S --noconfirm perl-template-toolkit
RUN pacman -S --noconfirm perl-yaml
RUN /usr/bin/vendor_perl/cpanm Regexp::Grammars

# flatbuffers
RUN pacman -S --noconfirm flatbuffers

# lmdb
RUN pacman -S --noconfirm lmdb

# secp256k1
RUN pacman -S --noconfirm libsecp256k1

# build strfry
RUN git submodule update --init 
RUN make setup-golpe 
RUN make -j4

EXPOSE 7777

ENTRYPOINT ["/app/strfry"]
CMD ["relay"]
