{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  buildInputs = with pkgs; [
    stdenv.cc
    ffmpeg
    espeak-ng
    pjsip
    pkg-config
    openssl
    alsa-lib
  ];
}
