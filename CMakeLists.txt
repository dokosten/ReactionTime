cmake_minimum_required(VERSION 3.16)
project(ReactionTime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows subsystem application (no console window)
add_executable(ReactionTime WIN32 main.cpp resource.rc)

# Link Windows libraries
target_link_libraries(ReactionTime PRIVATE winmm)

# Optimization flags for Release builds
if(MSVC)
    target_compile_options(ReactionTime PRIVATE
        $<$<CONFIG:Release>:/O2 /Ob2 /DNDEBUG>
    )
endif()
