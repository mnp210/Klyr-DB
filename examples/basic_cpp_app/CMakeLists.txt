cmake_minimum_required(VERSION 3.20)
project(basic_cpp_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Consumer searches for KlyrDB package installed on the system or specified via -DCMAKE_PREFIX_PATH
find_package(KlyrDB REQUIRED)

add_executable(basic_cpp_app main.cpp)
target_link_libraries(basic_cpp_app PRIVATE KlyrDB::KlyrDB)
