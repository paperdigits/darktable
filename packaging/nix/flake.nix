{
  description = "A flake for building darktable";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
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
      pkgs = nixpkgs.legacyPackages.${system};
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
      libsoup_2_4
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
       cmakeFlags =
         [
           "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
           "-DBUILD_USERMANUAL=False"
    ];
    # ++ lib.optionals stdenv.hostPlatform.isDarwin [
    #   "-DUSE_COLORD=OFF"
    #   "-DUSE_KWALLET=OFF"
    # ];


    in
    {
      devShells.default = pkgs.mkShell {
        inherit buildInputs nativeBuildInputs cmakeFlags;


      };

      packages.default = pkgs.stdenv.mkDerivation {
        inherit buildInputs nativeBuildInputs cmakeFlags pname version src;


      };
    });
}
