{ pkgs ? import <nixpkgs> {} }:
with pkgs;
mkShell {
  buildInputs = [ 
    perl perlPackages.YAML perlPackages.TemplateToolkit
    lmdb zstd secp256k1 flatbuffers zlib openssl libuv
  ];
}
