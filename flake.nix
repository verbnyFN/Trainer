{
  description = "Algorithm Trainer development environment";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { nixpkgs, ... }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      pythonRuntimeClosure = pkgs.closureInfo {
        rootPaths = [ pkgs.python3 ];
      };
      cppRuntimeClosure = pkgs.closureInfo {
        rootPaths = [ pkgs.clang pkgs.gcc.cc ];
      };
      nsjailWithVersion = pkgs.writeShellScriptBin "nsjail" ''
        if [ "$#" -eq 1 ] && [ "$1" = "--version" ]; then
          echo "nsjail ${pkgs.lib.getVersion pkgs.nsjail}"
          exit 0
        fi
        exec ${pkgs.nsjail}/bin/nsjail "$@"
      '';
    in
    {
      devShells.${system}.default = pkgs.mkShell.override {
        stdenv = pkgs.clangStdenv;
      } {
        packages = with pkgs; [
          catch2_3
          clang
          clang-tools
          cmake
          curl
          drogon
          git
          ninja
          nodejs
          nsjailWithVersion
          pkg-config
          pnpm
          postgresql
          python3
          libsodium
          sqlite
        ];

        ALGORITHM_TRAINER_NSJAIL_PATH = "${pkgs.nsjail}/bin/nsjail";
        ALGORITHM_TRAINER_PYTHON_PATH = "${pkgs.python3}/bin/python3";
        ALGORITHM_TRAINER_PYTHON_RUNTIME_CLOSURE = "${pythonRuntimeClosure}/store-paths";
        ALGORITHM_TRAINER_CPP_COMPILER_PATH = "${pkgs.clang}/bin/clang++";
        ALGORITHM_TRAINER_CPP_RUNTIME_CLOSURE = "${cppRuntimeClosure}/store-paths";
      };
    };
}
