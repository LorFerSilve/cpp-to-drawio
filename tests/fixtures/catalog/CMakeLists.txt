cmake_minimum_required(VERSION 3.21)
project(drawio_catalog LANGUAGES CXX)

add_library(order OBJECT src/order.cpp)
target_include_directories(order PRIVATE include)
set_target_properties(order PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

add_library(audit OBJECT src/audit.cpp)
target_include_directories(audit PRIVATE include "include options")
target_compile_definitions(audit PRIVATE WITH_AUDIT=1)
set_target_properties(audit PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
