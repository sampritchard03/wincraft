cmake_minimum_required(VERSION 3.17)
project(wincraft_native LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# template_debug is what the editor and debug runs load; template_release is for exports.
set(GODOTCPP_TARGET "template_debug" CACHE STRING "")

find_package(Threads REQUIRED)
add_subdirectory(godot-cpp SYSTEM)

add_library(wincraft_native SHARED
    src/register_types.cpp
    src/sculpt_terrain.cpp
    src/clipmap.cpp
    src/voxel_world.cpp
    src/noise.cpp
    src/density_body.cpp
    src/density_shape.cpp
    src/selection.cpp
)
target_link_libraries(wincraft_native PRIVATE godot-cpp Threads::Threads)

if(WIN32)
    set(WINCRAFT_PLATFORM windows)
    if(MINGW)
        # Self-contained DLL: no libstdc++ / libgcc / winpthread DLLs needed beside it.
        target_link_options(wincraft_native PRIVATE -static)
    endif()
else()
    set(WINCRAFT_PLATFORM linux)
endif()

set_target_properties(wincraft_native PROPERTIES
    CXX_STANDARD 17
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON
    PREFIX "lib"
    OUTPUT_NAME "wincraft_native.${WINCRAFT_PLATFORM}.${GODOTCPP_TARGET}.x86_64"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)
