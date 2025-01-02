# This is a nix flake that provides a development environment for NebulaStream.
# C++ dependencies are managed via vcpkg.
# The recommended way is to use our docker development container, but if you prefer to use nix, you can use this flake.
# Use `nix develop` to enter the development environment and start your IDE from there.

{
  description = "Nix dev environment for NebulaStream";
  inputs = {
    nixpkgs.url = "nixpkgs";
  };

  outputs = all@{ self, nixpkgs, ... }: {
    # Utilized by `nix develop`
    devShells.x86_64-linux.default =
      let
        pkgs = import nixpkgs {
          system = "x86_64-linux";
        };
      in
      with pkgs;
      mkShell {
        buildInputs = [
            cmake
            ccache
            vcpkg
	        clang_18
            llvm_18
	        mold
	        ninja

	        antlr4

            valgrind
        ];
	    nativeBuildInputs = with pkgs; [
            pkg-config
    	];

    	#cmakeFlags = cmakeFlags ++ [
        #    "CMAKE_CXX_COMPILER=${clang_18}/bin/clang++"
        #];
	        CC = "${clang_18}/bin/clang";
        CXX = "${clang_18}/bin/clang++";

        # Do not build MLIR from source
        USE_LOCAL_MLIR = "ON";

        # Setup vcpkg
        shellHook = ''
          export VCPKG_ROOT="${vcpkg}/share/vcpkg/"
          export VCPKG_TOOLCHAIN_FILE="${vcpkg}/share/vcpkg/scripts/buildsystems/vcpkg.cmake"
        '';

	    LD_LIBRARY_PATH = "${stdenv.cc.cc.lib}/lib";

	    # not sure if this is needed
	    CMAKE_PREFIX_PATH = [
            "${clang_18}"
            "${llvm_18}"
        ];
      };
  };
}
