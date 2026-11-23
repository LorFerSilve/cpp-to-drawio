cmake_minimum_required(VERSION 3.21)
project(cpp_to_drawio VERSION 0.1.0 LANGUAGES C CXX)

find_package(LLVM REQUIRED CONFIG)
if(LLVM_VERSION_MAJOR LESS 18)
    message(FATAL_ERROR "LLVM and Clang 18 or newer are required")
endif()
find_package(Clang REQUIRED CONFIG)
find_package(tinyxml2 QUIET CONFIG)
if(NOT TARGET tinyxml2::tinyxml2)
    find_path(TINYXML2_INCLUDE_DIR tinyxml2.h REQUIRED)
    find_library(TINYXML2_LIBRARY NAMES tinyxml2 REQUIRED)
    add_library(tinyxml2::tinyxml2 UNKNOWN IMPORTED)
    set_target_properties(tinyxml2::tinyxml2 PROPERTIES
        IMPORTED_LOCATION "${TINYXML2_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${TINYXML2_INCLUDE_DIR}")
endif()
find_package(ZLIB REQUIRED)

find_program(CLANG_DRIVER NAMES clang clang-${LLVM_VERSION_MAJOR}
    HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
execute_process(COMMAND "${CLANG_DRIVER}" -print-resource-dir
    OUTPUT_VARIABLE CLANG_RESOURCES OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
file(TO_CMAKE_PATH "${CLANG_RESOURCES}" CLANG_RESOURCES)
get_filename_component(CLANG_RESOURCE_VERSION "${CLANG_RESOURCES}" NAME)

add_executable(cpp-to-drawio
    src/main.cpp
    src/paths.cpp
    src/uml.cpp
    src/analyzer.cpp
    src/diagram_layout.cpp
    src/drawio_writer.cpp
    src/cpp_syntax.cpp
    src/drawio_reader.cpp
    src/cpp_skeletons.cpp
    src/skeleton_doxygen.cpp
)
# Recent Clang releases themselves require C++20 for consumers of their headers.
target_compile_features(cpp-to-drawio PRIVATE cxx_std_20)
set_target_properties(cpp-to-drawio PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(cpp-to-drawio SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
separate_arguments(LLVM_COMPILE_OPTIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_options(cpp-to-drawio PRIVATE ${LLVM_COMPILE_OPTIONS})
target_compile_definitions(cpp-to-drawio PRIVATE
    DRAWIO_CLANG_RESOURCES="${CLANG_RESOURCES}"
    DRAWIO_CLANG_RESOURCE_VERSION="${CLANG_RESOURCE_VERSION}")
if(TARGET clang-cpp AND TARGET LLVM)
    target_link_libraries(cpp-to-drawio PRIVATE clang-cpp LLVM)
else()
    target_link_libraries(cpp-to-drawio PRIVATE clangTooling clangAST clangFrontend clangBasic clangStaticAnalyzerFrontend LLVMSupport)
endif()
target_link_libraries(cpp-to-drawio PRIVATE tinyxml2::tinyxml2 ZLIB::ZLIB)
if(MSVC)
    target_compile_options(cpp-to-drawio PRIVATE /utf-8 /W4)
else()
    target_compile_options(cpp-to-drawio PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(REVERSE_EXECUTABLE "$<TARGET_FILE_DIR:cpp-to-drawio>/drawio-to-cpp${CMAKE_EXECUTABLE_SUFFIX}")
add_custom_command(TARGET cpp-to-drawio POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:cpp-to-drawio>" "${REVERSE_EXECUTABLE}"
    VERBATIM)
if(WIN32)
    install(CODE [[
        if(POLICY CMP0207)
            cmake_policy(SET CMP0207 NEW)
        endif()
    ]])
    install(TARGETS cpp-to-drawio RUNTIME_DEPENDENCY_SET drawio_runtime RUNTIME DESTINATION bin)
    install(RUNTIME_DEPENDENCY_SET drawio_runtime
        DESTINATION bin
        DIRECTORIES "${LLVM_TOOLS_BINARY_DIR}"
        PRE_EXCLUDE_REGEXES "^[Aa][Pp][Ii]-[Mm][Ss]-" "^[Ee][Xx][Tt]-[Mm][Ss]-"
        POST_EXCLUDE_REGEXES "[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]")
    foreach(dependency clang clang-libs llvm llvm-libs libc++ libffi libiconv libxml2 libunwind libwinpthread tinyxml2 zlib zstd)
        install(DIRECTORY "${LLVM_TOOLS_BINARY_DIR}/../share/licenses/${dependency}"
            DESTINATION share/licenses OPTIONAL)
    endforeach()
else()
    install(TARGETS cpp-to-drawio RUNTIME DESTINATION bin)
endif()
install(CODE "
    file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/bin\" TYPE PROGRAM
        FILES \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/cpp-to-drawio${CMAKE_EXECUTABLE_SUFFIX}\"
        RENAME \"drawio-to-cpp${CMAKE_EXECUTABLE_SUFFIX}\")
")
install(DIRECTORY "${CLANG_RESOURCES}/include" DESTINATION "lib/clang/${CLANG_RESOURCE_VERSION}")
install(FILES LICENSE DESTINATION share/cpp-to-drawio)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    find_program(CLANG_CXX_DRIVER NAMES clang++ clang++-${LLVM_VERSION_MAJOR}
        HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
    add_test(NAME forward-cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/forward_cli.py" "$<TARGET_FILE:cpp-to-drawio>" "${CLANG_CXX_DRIVER}")
    add_test(NAME reverse-cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/reverse_cli.py" "$<TARGET_FILE:cpp-to-drawio>" "${CLANG_CXX_DRIVER}"
        "${REVERSE_EXECUTABLE}")
endif()
