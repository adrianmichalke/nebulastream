# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at

#    https://www.apache.org/licenses/LICENSE-2.0

# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

include_directories(tests/include)

# Set Sources and Headers
add_subdirectory(src)
get_source(nes-nautilus NES_NAUTILUS_SOURCE_FILES)

# Add Library
add_library(nes-nautilus ${NES_NAUTILUS_SOURCE_FILES})
target_link_libraries(nes-nautilus PUBLIC nes-memory)
target_include_directories(nes-nautilus PUBLIC ${CMAKE_SOURCE_DIR}/nes-runtime/include)

# Set C++20 standard for reflect-cpp
target_compile_features(nes-nautilus PUBLIC cxx_std_20)


if (NES_ENABLE_PRECOMPILED_HEADERS)
    target_precompile_headers(nes-nautilus REUSE_FROM nes-common)
    # We need to compile with -fPIC to include with nes-common compiled headers as it uses PIC
    target_compile_options(nes-nautilus PUBLIC "-fPIC")
endif ()

add_tests_if_enabled(tests)

# Add MLIR and LLVM dependencies
if (NES_ENABLE_EXPERIMENTAL_EXECUTION_MLIR)
    # include MLIR. This is currently required
    find_package(MLIR REQUIRED CONFIG)
    if (MLIR_FOUND)
        message(STATUS MLIR_INCLUDE_DIRS=${MLIR_INCLUDE_DIRS})
        target_include_directories(nes-nautilus SYSTEM PUBLIC
                $<BUILD_INTERFACE:${MLIR_INCLUDE_DIRS}>
                $<INSTALL_INTERFACE:include/nebulastream/>)
        add_compile_definitions(MLIR_COMPILER)
        target_link_libraries(nes-nautilus PRIVATE
                MLIRExecutionEngine
                MLIRFuncAllExtensions

                # Dialects
                MLIRFuncToLLVM
        )

    else ()
        message(FATAL_ERROR "Cannot find mlir")
    endif (MLIR_FOUND)
    target_compile_definitions(nes-nautilus PUBLIC USE_MLIR)

    # If we want to support inlining we have to compile the llvm ir for the provided headers.
    # This is currently only supported on Ubuntu 22.04
    if (NES_ENABLE_EXPERIMENTAL_EXECUTION_MLIR_INLINING)
        add_library(nes-runtime-proxy ${CMAKE_CURRENT_SOURCE_DIR}/../nes-runtime/src/Runtime/detail/TupleBufferImpl.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../nes-runtime/src/Execution/TupleBufferProxyFunctions.cpp)
        message(STATUS "CMAKE_SOURCE_DIR ${CMAKE_SOURCE_DIR}")
        target_include_directories(nes-runtime-proxy PUBLIC ${CMAKE_SOURCE_DIR}/nes-common/include/ ${CMAKE_SOURCE_DIR}/nes-common/include/ ${CMAKE_SOURCE_DIR}/nes-runtime/include/ ${CMAKE_SOURCE_DIR}/nes-data-types/include/)

        set_target_properties(nes-runtime-proxy PROPERTIES LINKER_LANGUAGE CXX)
        llvmir_attach_bc_target(nes-runtime_bc nes-runtime-proxy -O3)
        add_dependencies(nes-runtime_bc nes-runtime-proxy)

        llvmir_attach_link_target(nes-runtime_link nes-runtime_bc)
        llvmir_attach_opt_pass_target(nes-runtime_opt nes-runtime_link -S -O3)

        get_target_property(OP_BINARY_DIR nes-runtime_opt LLVMIR_DIR)
        get_target_property(OP_BINARY_LLVMIR_FILES nes-runtime_opt LLVMIR_FILES)

        message(STATUS "CMAKE_CURRENT_SOURCE_DIR_EXECUTION: ${CMAKE_CURRENT_SOURCE_DIR}")
        message(STATUS "OP_BINARY_DIR: ${OP_BINARY_DIR}")
        message(STATUS "OP_BINARY_LLVMIR_FILES: ${OP_BINARY_LLVMIR_FILES}")

        # Reducing linked & optimized LLVM IR to Proxy Functions only.
        add_executable(generate-proxy-function-ir "${CMAKE_CURRENT_SOURCE_DIR}/src/Experimental/Utility/ExtractFunctionsFromLLVMIR.cpp")
        set_target_properties(generate-proxy-function-ir PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OP_BINARY_DIR}")
        list(POP_FRONT OP_BINARY_LLVMIR_FILES OP_BINARY_LLVMIR_FILE)
        target_compile_definitions(nes-runtime-proxy PRIVATE PROXY_FUNCTIONS_RESULT_DIR_CMAKE=\"${OP_BINARY_DIR}\")

        target_compile_definitions(generate-proxy-function-ir PRIVATE IR_FILE_DIR=\"${OP_BINARY_DIR}\")
        target_compile_definitions(generate-proxy-function-ir PRIVATE IR_FILE_FILE=\"${OP_BINARY_LLVMIR_FILE}\")
        target_compile_definitions(generate-proxy-function-ir PRIVATE PROXY_FUNCTIONS_RESULT_DIR_CMAKE=\"${OP_BINARY_DIR}\")

        target_link_libraries(generate-proxy-function-ir ${mlir_libs})
        target_link_libraries(generate-proxy-function-ir ${llvm_libs})
        target_include_directories(generate-proxy-function-ir SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})

        add_custom_command(TARGET generate-proxy-function-ir POST_BUILD
                COMMAND "${OP_BINARY_DIR}/generate-proxy-function-ir"
                COMMAND "echo" ARGS "Executing function reducer on linked and optimized LLVM IR."
                VERBATIM)
        add_dependencies(generate-proxy-function-ir nes-runtime_opt)
        add_dependencies(nes-runtime-proxy generate-proxy-function-ir)
    endif ()
endif ()

# Add nes-nautilus to the include directories
target_include_directories(nes-nautilus PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include/nebulastream/>
)

# Add reflect-cpp for automatic serialization (prefer Nix package over FetchContent)
if(DEFINED ENV{NIX_BUILD_TOP})
    # Using Nix environment - find the package
    find_package(reflectcpp REQUIRED)
    find_package(CapnProto REQUIRED)
else()
    # Fallback to FetchContent for non-Nix builds
    include(FetchContent)
    FetchContent_Declare(
        reflect-cpp
        GIT_REPOSITORY https://github.com/getml/reflect-cpp.git
        GIT_TAG v0.20.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(reflect-cpp)
endif()

# Link with the nautilus library
find_package(spdlog REQUIRED CONFIG)
find_package(MLIR REQUIRED CONFIG)

# Handle local nautilus build vs vcpkg
if (USE_LOCAL_NAUTILUS)
    # When using local nautilus, try to find it in a few common locations
    if (DEFINED ENV{NAUTILUS_DIR})
        # Try the install directory first if it exists
        if (EXISTS "$ENV{NAUTILUS_DIR}/install/lib64/cmake")
            list(APPEND CMAKE_PREFIX_PATH "$ENV{NAUTILUS_DIR}/install")
            message(STATUS "Looking for installed nautilus at: $ENV{NAUTILUS_DIR}/install")
        elseif (EXISTS "$ENV{NAUTILUS_DIR}/install/lib/cmake")
            list(APPEND CMAKE_PREFIX_PATH "$ENV{NAUTILUS_DIR}/install")
            message(STATUS "Looking for installed nautilus at: $ENV{NAUTILUS_DIR}/install")
        else()
            # Fall back to direct config path
            set(nautilus_DIR "$ENV{NAUTILUS_DIR}/build/nautilus/CMakeFiles/Export/06c502cb36a19255158111b9780c4eb3")
            message(STATUS "Looking for nautilus build at: $ENV{NAUTILUS_DIR}/build")
        endif()
    endif()
    # Also try the sibling directory (common development setup)
    if (EXISTS "${CMAKE_SOURCE_DIR}/../nautilus/install/lib64/cmake")
        list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/../nautilus/install")
        message(STATUS "Using installed sibling nautilus")
    elseif (EXISTS "${CMAKE_SOURCE_DIR}/../nautilus/install/lib/cmake")
        list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/../nautilus/install")
        message(STATUS "Using installed sibling nautilus")
    else()
        set(nautilus_DIR "${CMAKE_SOURCE_DIR}/../nautilus/build/nautilus/CMakeFiles/Export/06c502cb36a19255158111b9780c4eb3")
        message(STATUS "Using sibling nautilus build directory")
    endif()
endif()

find_package(nautilus REQUIRED CONFIG)

# We have to set the nes-nautilus to public to use the nautilus library in other modules
if(DEFINED ENV{NIX_BUILD_TOP})
    target_link_libraries(nes-nautilus PUBLIC nautilus::nautilus
            reflectcpp::reflectcpp
            CapnProto::capnp
            MLIRExecutionEngine
            MLIRFuncAllExtensions

            # Dialects
            MLIRSCFDialect
            MLIRFuncToLLVM
            MLIRSCFToControlFlow
    )
else()
    target_link_libraries(nes-nautilus PUBLIC nautilus::nautilus
            reflectcpp::reflectcpp
            MLIRExecutionEngine
            MLIRFuncAllExtensions

            # Dialects
            MLIRSCFDialect
            MLIRFuncToLLVM
            MLIRSCFToControlFlow
    )
endif()

# Add nautilus include subdirectory for compatibility - using Nix-provided nautilus
# This allows includes like <val.hpp> to work instead of requiring <nautilus/val.hpp>
get_target_property(NAUTILUS_INCLUDE_DIRS nautilus::nautilus INTERFACE_INCLUDE_DIRECTORIES)
if(NAUTILUS_INCLUDE_DIRS)
    foreach(include_dir ${NAUTILUS_INCLUDE_DIRS})
        if(EXISTS "${include_dir}/nautilus")
            target_include_directories(nes-nautilus PUBLIC "${include_dir}/nautilus")
            message(STATUS "Added nautilus header compatibility path from Nix: ${include_dir}/nautilus")
            break()
        endif()
    endforeach()
endif()

#Add the nes nautilus and headers to cpack
install(TARGETS nes-nautilus
        EXPORT NebulaStreamTargets
        COMPONENT nesLibrary
        DESTINATION lib)
