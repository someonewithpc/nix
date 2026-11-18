{
  stdenv,
  lib,
  mkMesonExecutable,

  nix-util,
  nix-store,
  nix-fetchers,
  nix-expr,
  nix-flake,
  nix-main,
  nix-cmd,

  nix-expr-c,
  nix-fetchers-c,
  nix-flake-c,
  nix-main-c,
  nix-store-c,
  nix-util-c,

  mimalloc,

  # Configuration Options

  version,

  # Whether to link against mimalloc for malloc override.
  # Significantly improves evaluation performance on allocation-heavy
  # workloads (~10-15% on large evaluations).
  # mimalloc is disabled on FreeBSD due to a crash in nixpkgs 25.11.
  # Once the nixpkgs flake is updated, mimalloc can be enabled again.
  withMimalloc ? !stdenv.hostPlatform.isWindows && !stdenv.hostPlatform.isFreeBSD,

  # Whether to embed the public C API into the `nix` executable so plugins can
  # resolve those symbols without linking Nix libraries directly.
  withPluginCApi ? !stdenv.hostPlatform.isWindows && !stdenv.hostPlatform.isStatic,

  # Whether to link the Nix libraries statically, which needs them built with
  # `default_library=both`. C++ plugins that link the shared libraries break.
  withStaticNixLibraries ? false,
}:

let
  inherit (lib) fileset;
in

mkMesonExecutable (finalAttrs: {
  pname = "nix";
  inherit version;

  workDir = ./.;
  fileset = fileset.unions (
    [
      ../../nix-meson-build-support
      ./nix-meson-build-support
      ../../.version
      ./.version
      ./meson.build
      ./meson.options

      # Symbolic links to other dirs
      ## exes
      ./doc
      ## dirs
      ./scripts
      ../../scripts
      ./misc
      ../../misc

      # Doc nix files for --help
      ../../doc/manual/generate-manpage.nix
      ../../doc/manual/utils.nix
      ../../doc/manual/generate-settings.nix
      ../../doc/manual/generate-store-info.nix

      # Other files to be included as string literals
      ./nix-channel/unpack-channel.nix
      ./nix-env/buildenv.nix
      ./get-env.sh
      ./help-stores.md
      ../../doc/manual/source/store/types/index.md.in
      ./profiles.md
      ../../doc/manual/source/command-ref/files/profiles.md

      # Files
    ]
    ++ [
      (fileset.fileFilter (file: file.hasExt "cc") ./.)
      (fileset.fileFilter (file: file.hasExt "hh") ./.)
      (fileset.fileFilter (file: file.hasExt "md") ./.)
    ]
  );

  buildInputs = [
    nix-store
    nix-expr
    nix-main
    nix-cmd
  ]
  ++ lib.optionals withPluginCApi [
    nix-expr-c
    nix-fetchers-c
    nix-flake-c
    nix-main-c
    nix-store-c
    nix-util-c
  ]
  # The static libraries need their private dependencies at link time
  ++ lib.optionals withStaticNixLibraries (
    lib.concatMap (p: p.buildInputs) [
      nix-util
      nix-store
      nix-fetchers
      nix-expr
      nix-flake
      nix-main
      nix-cmd
    ]
  )
  ++ lib.optional withMimalloc mimalloc;

  mesonFlags = [
    (lib.mesonEnable "mimalloc" withMimalloc)
    (lib.mesonBool "plugin-c-api" withPluginCApi)
    (lib.mesonBool "static-nix-libraries" withStaticNixLibraries)
  ];

  postInstall = lib.optionalString stdenv.hostPlatform.isStatic ''
    mkdir -p $out/nix-support
    echo "file binary-dist $out/bin/nix" >> $out/nix-support/hydra-build-products
  '';

  passthru = {
    exportsPluginCApi = withPluginCApi;
    linksNixLibrariesStatically = withStaticNixLibraries;
  };

  meta = {
    mainProgram = "nix";
    platforms = lib.platforms.unix ++ lib.platforms.windows;
  };

})
