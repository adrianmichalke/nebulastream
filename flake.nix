# This is a nix flake that provides a development environment for NebulaStream.
# C++ dependencies are managed via vcpkg.
# The recommended way is to use our docker development container, but if you prefer to use nix, you can use this flake.
# Use `nix develop` to enter the development environment and start your IDE from there.

{
  description = "Nix dev environment for NebulaStream";
  inputs = {
    nixpkgs.url = "nixpkgs";
    nautilus = {
      url = "path:/home/adrian/workspace/nautilus";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = all@{ self, nixpkgs, nautilus, ... }: {
    # Utilized by `nix develop`
    devShells.x86_64-linux.default =
      let
        pkgs = import nixpkgs {
          system = "x86_64-linux";
        };
        
        # Use nautilus from its own flake
        nautilusPackage = nautilus.packages.x86_64-linux.default;
        
        # Custom libcuckoo derivation (header-only library)
        libcuckoo = pkgs.stdenv.mkDerivation rec {
          pname = "libcuckoo";
          version = "0.3.1";
          
          src = pkgs.fetchFromGitHub {
            owner = "efficient";
            repo = "libcuckoo";
            rev = "v${version}";
            sha256 = "sha256-NAXP5Rag6thFnoqNa91dd/DlNRN6GW/MbCOgCeeFPkE=";
          };
          
          nativeBuildInputs = [ pkgs.cmake ];
          
          # Header-only library, just need to install headers and cmake config
          installPhase = ''
            mkdir -p $out/include
            cp -r $src/libcuckoo $out/include/
            
            # Create basic CMake config
            mkdir -p $out/lib/cmake/libcuckoo
            cat > $out/lib/cmake/libcuckoo/libcuckooConfig.cmake << EOF
            # libcuckoo CMake config
            if(NOT TARGET libcuckoo::libcuckoo)
              add_library(libcuckoo::libcuckoo INTERFACE IMPORTED)
              target_include_directories(libcuckoo::libcuckoo INTERFACE "\''${CMAKE_CURRENT_LIST_DIR}/../../../include")
            endif()
            EOF
          '';
        };
        
        # Custom reflect-cpp derivation - with compiled Cap'n Proto support
        reflect-cpp = pkgs.stdenv.mkDerivation rec {
          pname = "reflect-cpp";
          version = "0.20.0";
          
          src = pkgs.fetchFromGitHub {
            owner = "getml";
            repo = "reflect-cpp";
            rev = "v${version}";
            sha256 = "sha256-ZIGKxGtarPVx81NHNKdMpGns7b7/6ygiziEP7XgyNaM=";
          };
          
          nativeBuildInputs = [ pkgs.cmake pkgs.pkg-config ];
          buildInputs = [ pkgs.capnproto ];
          
          cmakeFlags = [
            "-DREFLECTCPP_BUILD_SHARED=OFF"
            "-DREFLECTCPP_CAPNPROTO=ON" 
            "-DREFLECTCPP_BUILD_TESTS=OFF"
            "-DREFLECTCPP_USE_VCPKG=OFF"
            "-DREFLECTCPP_USE_BUNDLED_DEPENDENCIES=ON"
            "-DREFLECTCPP_INSTALL=ON"
          ];
          
          buildPhase = ''
            runHook preBuild
            make -j$NIX_BUILD_CORES
            runHook postBuild
          '';
          
          installPhase = ''
            runHook preInstall
            make install
            
            # Update CMake config to ensure Cap'n Proto is linked properly
            # Update CMake config to point to the built library and link Cap'n Proto
            if [[ -f "$out/lib/cmake/reflectcpp/reflectcppConfig.cmake" ]]; then
              echo "# Additional Cap'n Proto linking" >> "$out/lib/cmake/reflectcpp/reflectcppConfig.cmake"
              cat >> "$out/lib/cmake/reflectcpp/reflectcppConfig.cmake" << 'EOF'
            find_package(PkgConfig QUIET)
            if(PkgConfig_FOUND)
              pkg_check_modules(CAPNP capnp)
              if(CAPNP_FOUND)
                target_link_libraries(reflectcpp INTERFACE ''${CAPNP_LIBRARIES})
                target_include_directories(reflectcpp INTERFACE ''${CAPNP_INCLUDE_DIRS})
              endif()
            endif()
            EOF
            fi
            
            runHook postInstall
          '';
        };
        
        
        # Build spdlog with fmt_11 for compatibility  
        spdlogWithFmt11 = pkgs.spdlog.overrideAttrs (oldAttrs: {
          buildInputs = (oldAttrs.buildInputs or []) ++ [pkgs.fmt_11];
          cmakeFlags = (oldAttrs.cmakeFlags or []) ++ [
            "-DSPDLOG_FMT_EXTERNAL=ON"
            "-DSPDLOG_BUILD_SHARED=ON"
          ];
        });
      in
      with pkgs;
      mkShell {
        buildInputs = [
            cmake
            ccache
            vcpkg
	        llvmPackages_19.clang
            llvmPackages_19.llvm
            llvmPackages_19.mlir
	        mold
	        ninja

	        antlr4
	        antlr4.runtime.cpp
	        
	        # Add lsb-release for OS detection
	        lsb-release

            valgrind
            
            # Dependencies typically provided by vcpkg
            openssl
            libffi
            libxml2
            zlib
            grpc
            protobuf
            abseil-cpp
            fmt_11
            spdlogWithFmt11
            gtest
            gbenchmark
            nlohmann_json
            yaml-cpp
            boost
            folly
            glog
            liburing
            
            # Add cpptrace for error handling
            cpptrace
            
            # Add missing C++ libraries  
            magic-enum
            argparse
            
            # Add our custom libcuckoo
            libcuckoo
            
            # Add reflect-cpp and Cap'n Proto for serialization
            reflect-cpp
            capnproto
            
            # Add replxx for nes-nebuli
            replxx
            
            # Add our local nautilus build
            nautilusPackage
        ];
	    nativeBuildInputs = with pkgs; [
            pkg-config
    	];

    	#cmakeFlags = cmakeFlags ++ [
        #    "CMAKE_CXX_COMPILER=${clang_18}/bin/clang++"
        #];
	        CC = "${llvmPackages_19.clang}/bin/clang";
        CXX = "${llvmPackages_19.clang}/bin/clang++";
        
        # Export to shell environment
        CMAKE_C_COMPILER = "${llvmPackages_19.clang}/bin/clang";
        CMAKE_CXX_COMPILER = "${llvmPackages_19.clang}/bin/clang++";

        # Do not build MLIR from source
        USE_LOCAL_MLIR = "ON";
        
        # Use local nautilus build instead of vcpkg
        USE_LOCAL_NAUTILUS = "ON";
        NAUTILUS_DIR = "/home/adrian/workspace/nautilus";

        # Setup vcpkg and compiler symlinks
        shellHook = ''
          export CC="${llvmPackages_19.clang}/bin/clang"
          export CXX="${llvmPackages_19.clang}/bin/clang++"
          export CMAKE_C_COMPILER="${llvmPackages_19.clang}/bin/clang"
          export CMAKE_CXX_COMPILER="${llvmPackages_19.clang}/bin/clang++"
          
          export VCPKG_ROOT="${vcpkg}/share/vcpkg/"
          export VCPKG_TOOLCHAIN_FILE="${vcpkg}/share/vcpkg/scripts/buildsystems/vcpkg.cmake"
          
          # Set LLVM version for vcpkg toolchain
          export LLVM_VERSION=18
          
          # Create temporary directory for compiler symlinks
          export CLANG_SYMLINK_DIR="$PWD/.nix-clang-symlinks"
          mkdir -p "$CLANG_SYMLINK_DIR"
          
          # Create version-specific symlinks for vcpkg (map 19 to 18 for compatibility)
          ln -sf "${llvmPackages_19.clang}/bin/clang" "$CLANG_SYMLINK_DIR/clang-18"
          ln -sf "${llvmPackages_19.clang}/bin/clang++" "$CLANG_SYMLINK_DIR/clang++-18"
          
          # Add symlink directory to PATH
          export PATH="$CLANG_SYMLINK_DIR:$PATH"
          
          # Add magic_enum include path for direct include
          export CPLUS_INCLUDE_PATH="${magic-enum}/include:$CPLUS_INCLUDE_PATH"
          export C_INCLUDE_PATH="${magic-enum}/include:$C_INCLUDE_PATH"
          
          # Set up local nautilus development
          export NAUTILUS_DIR="/home/adrian/workspace/nautilus"
          export USE_LOCAL_NAUTILUS="ON"
          if [ -d "$NAUTILUS_DIR" ]; then
            echo "Using local nautilus at: $NAUTILUS_DIR"
          else
            echo "Warning: nautilus directory not found at $NAUTILUS_DIR"
            echo "Make sure nautilus is located at the expected path or set NAUTILUS_DIR manually"
          fi
          
          # Set Nautilus from Nix package path
          export NAUTILUS_NIX_PATH="${nautilusPackage}"
          
          echo "LLVM 18 compiler symlinks created in $CLANG_SYMLINK_DIR"
        '';

        LD_LIBRARY_PATH = lib.makeLibraryPath ([
            stdenv.cc.cc.lib
            yaml-cpp
            grpc
            protobuf
            abseil-cpp
            fmt_11
            spdlogWithFmt11
            folly
            glog
            gflags
            boost
            openssl
            zlib
            zstd
            cpptrace
            liburing
            libffi
            re2
            c-ares
            xz
            lz4
            libunwind
            double-conversion
            libevent
            icu
            gtest
            capnproto
            nautilusPackage
        ] ++ (with llvmPackages_19; [
            llvm
            mlir
        ]));

	    # CMake and pkg-config paths for all dependencies
        CMAKE_PREFIX_PATH = with pkgs; lib.makeSearchPath "lib/cmake" [
            llvmPackages_19.clang
            llvmPackages_19.llvm
            llvmPackages_19.mlir
            folly
            boost
            glog
            abseil-cpp
            magic-enum
            fmt_11
            spdlogWithFmt11
            yaml-cpp
            nlohmann_json
            gtest
            gbenchmark
            cpptrace
            openssl
            zlib
            grpc
            protobuf
            antlr4
            antlr4.runtime.cpp
            argparse
            libcuckoo
            reflect-cpp
            capnproto
            llvmPackages_19.libcxx
        ] + ":" + lib.makeSearchPath "" [
            "${llvmPackages_19.clang}"
            "${llvmPackages_19.llvm}"
            "${llvmPackages_19.mlir}"
        ];
        
        PKG_CONFIG_PATH = with pkgs; lib.makeSearchPath "lib/pkgconfig" [
            folly
            boost
            glog
            abseil-cpp
            fmt_11
            spdlogWithFmt11
            yaml-cpp
            nlohmann_json
            openssl
            zlib
            protobuf
            capnproto
            liburing
        ];
      };
  };
}
