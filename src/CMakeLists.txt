# Enable C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Collect all .cpp sources only (NO .c mixing!)
file(GLOB_RECURSE app_sources
    ${CMAKE_SOURCE_DIR}/src/*.cpp
)

# file(GLOB_RECURSE app_sources *.cpp)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/src"
)
