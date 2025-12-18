# Enable C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Collect all .cpp sources only (NO .c mixing!)
file(GLOB_RECURSE app_sources
    ${CMAKE_SOURCE_DIR}/src/*.cpp
    ${CMAKE_SOURCE_DIR}/lib/**/*.cpp
)

# file(GLOB_RECURSE app_sources *.cpp)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS 
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/lib"
        "${CMAKE_SOURCE_DIR}/lib/audio"
        "${CMAKE_SOURCE_DIR}/lib/display"
        "${CMAKE_SOURCE_DIR}/lib/network"
        "${CMAKE_SOURCE_DIR}/lib/power"
        "${CMAKE_SOURCE_DIR}/lib/touch"
)
