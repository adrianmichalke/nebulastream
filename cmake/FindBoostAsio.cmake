# FindBoostAsio.cmake - NebulaStream compatibility wrapper
# This module provides Boost::asio target for Nix compatibility
#
# Boost::asio is header-only but depends on system and thread components.
# The Nix boost package doesn't provide a separate asio component, so we
# create it by combining the necessary dependencies.

# First ensure we have the required Boost components
find_package(Boost REQUIRED COMPONENTS system thread)

if(Boost_FOUND AND NOT TARGET Boost::asio)
    # Create the Boost::asio interface target
    add_library(Boost::asio INTERFACE IMPORTED)
    
    # asio is header-only but depends on system and thread for some functionality
    set_target_properties(Boost::asio PROPERTIES
        INTERFACE_LINK_LIBRARIES "Boost::system;Boost::thread"
    )
    
    # Include Boost headers if available
    if(TARGET Boost::headers)
        target_link_libraries(Boost::asio INTERFACE Boost::headers)
    elseif(TARGET Boost::boost)
        target_link_libraries(Boost::asio INTERFACE Boost::boost)
    endif()
    
    # Set include directories if available
    if(Boost_INCLUDE_DIRS)
        set_target_properties(Boost::asio PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIRS}"
        )
    endif()
    
    message(STATUS "Created Boost::asio interface target")
    message(STATUS "Boost::asio depends on: Boost::system, Boost::thread")
    
    # Mark as found
    set(BoostAsio_FOUND TRUE)
    set(BOOST_ASIO_FOUND TRUE)
else()
    set(BoostAsio_FOUND FALSE)
    set(BOOST_ASIO_FOUND FALSE)
    if(BoostAsio_FIND_REQUIRED)
        message(FATAL_ERROR "Could not find Boost components required for asio")
    elseif(NOT BoostAsio_FIND_QUIETLY)
        message(STATUS "Could not find Boost components required for asio")
    endif()
endif()