{
  description = "A flake for building darktable";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.11";
    systems.url = "github:nix-systems/x86_64-linux";
    flake-utils = {
      url = "github:numtide/flake-utils";
      inputs.systems.follows = "systems";
    };
  };

  outputs =
    { self
    , nixpkgs
    , flake-utils
    , ...
    }:
    flake-utils.lib.eachDefaultSystem (system:
    let
      #pkgs = nixpkgs.legacyPackages.${system};
      pkgs = nixpkgs.legacyPackages.x86_64-linux;
      pname = "darktable-git"; #package name
      version = "master";
      src = ../..;
      buildInputs = with pkgs; [
      SDL2
      adwaita-icon-theme
      cairo
      curl
      exiv2
      glib
      glib-networking
      gmic
      graphicsmagick
      gtk3
      icu
      ilmbase
      isocodes
      jasper
      json-glib
      lcms2
      lensfun
      libaom
      libavif
      libexif
      libgphoto2
      libheif
      libjpeg
      libjxl
      libpng
      librsvg
      libsecret
      libsoup
      libtiff
      libwebp
      libxslt
      lua
      openexr_3
      openjpeg
      osm-gps-map
      pcre
      portmidi
      pugixml
      sqlite

      ]
       ++ lib.optionals stdenv.hostPlatform.isLinux [
      colord
      colord-gtk
      #libX11
      ocl-icd
    ]
    ++ lib.optional stdenv.hostPlatform.isDarwin gtk-mac-integration
    ++ lib.optional stdenv.cc.isClang llvmPackages.openmp;
      nativeBuildInputs = with pkgs; [
    cmake
    desktop-file-utils
    intltool
    llvmPackages.llvm
    ninja
    perl
    pkg-config
    wrapGAppsHook3
      ];
    in
    {
      devShells.default = pkgs.mkShell {
        inherit buildInputs nativeBuildInputs;

      };

      # Pinned gcc: remain on gcc10 even after `nix flake update`
      #default = pkgs.mkShell.override { stdenv = pkgs.gcc10Stdenv; } {
      #  inherit buildInputs nativeBuildInputs;
      #};

      # Clang example:
      #default = pkgs.mkShell.override { stdenv = pkgs.clangStdenv; } {
      #  inherit buildInputs nativeBuildInputs;
      #};

      packages.default = pkgs.stdenv.mkDerivation {
        inherit buildInputs nativeBuildInputs pname version src;


      };
    });
}
