# Findfolly.cmake - NebulaStream compatibility wrapper
# This module provides lowercase folly:: targets for Nix compatibility
#
# The Nix folly package provides Folly::folly (capital F) but NebulaStream
# expects folly::folly (lowercase f). This wrapper creates the needed aliases.

# Ensure glog is available for folly
find_package(glog CONFIG QUIET)

# Find the Nix-provided Folly package (capital F)
find_package(Folly CONFIG QUIET)

if(Folly_FOUND)
    # Create lowercase aliases for NebulaStream compatibility
    if(TARGET Folly::folly AND NOT TARGET folly::folly)
        add_library(folly::folly ALIAS Folly::folly)
        message(STATUS "Created alias folly::folly -> Folly::folly")
    endif()
    
    if(TARGET Folly::folly_deps AND NOT TARGET folly::folly_deps)
        add_library(folly::folly_deps ALIAS Folly::folly_deps)
        message(STATUS "Created alias folly::folly_deps -> Folly::folly_deps")
    endif()
    
    if(TARGET Folly::folly_test_util AND NOT TARGET folly::folly_test_util)
        add_library(folly::folly_test_util ALIAS Folly::folly_test_util)
        message(STATUS "Created alias folly::folly_test_util -> Folly::folly_test_util")
    endif()
    
    if(TARGET Folly::follybenchmark AND NOT TARGET folly::follybenchmark)
        add_library(folly::follybenchmark ALIAS Folly::follybenchmark)
        message(STATUS "Created alias folly::follybenchmark -> Folly::follybenchmark")
    endif()
    
    # Set folly as found with appropriate variables
    set(folly_FOUND TRUE PARENT_SCOPE)
    set(folly_VERSION ${Folly_VERSION} PARENT_SCOPE)
    set(FOLLY_FOUND TRUE PARENT_SCOPE)
    set(FOLLY_VERSION ${Folly_VERSION} PARENT_SCOPE)
    
    # Also set them locally
    set(folly_FOUND TRUE)
    set(folly_VERSION ${Folly_VERSION})
    set(FOLLY_FOUND TRUE)
    set(FOLLY_VERSION ${Folly_VERSION})
    
    message(STATUS "Found folly via Nix: ${folly_VERSION}")
else()
    set(folly_FOUND FALSE PARENT_SCOPE)
    set(FOLLY_FOUND FALSE PARENT_SCOPE)
    set(folly_FOUND FALSE)
    set(FOLLY_FOUND FALSE)
    if(folly_FIND_REQUIRED)
        message(FATAL_ERROR "Could not find folly package")
    elseif(NOT folly_FIND_QUIETLY)
        message(STATUS "Could not find folly package")
    endif()
endif()