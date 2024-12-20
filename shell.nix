{ pkgs ? import <nixpkgs> {} }:
with pkgs;
mkShell {
  buildInputs = [ 
    perl lmdb zstd secp256k1 flatbuffers_23 zlib openssl libuv
  ];
}
