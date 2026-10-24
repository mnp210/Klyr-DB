cmake_minimum_required(VERSION 3.20)
project(transactions_and_constraints LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(KlyrDB REQUIRED)

add_executable(transactions_and_constraints main.cpp)
target_link_libraries(transactions_and_constraints PRIVATE KlyrDB::KlyrDB)
