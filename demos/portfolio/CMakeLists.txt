cmake_minimum_required(VERSION 3.20)
project(klyrdb_portfolio_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find installed KlyrDB package (pass -DCMAKE_PREFIX_PATH="path/to/install")
find_package(KlyrDB REQUIRED)

add_executable(demo_sdk demo_sdk.cpp)
target_link_libraries(demo_sdk PRIVATE KlyrDB::KlyrDB)
