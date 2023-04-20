{ pkgs ? import <nixpkgs> {} }:
with pkgs;
mkShell {
  buildInputs = [ 
    perl perlPackages.YAML perlPackages.TemplateToolkit
    lmdb zstd secp256k1 libb2 flatbuffers zlib openssl libuv
  ];
}
