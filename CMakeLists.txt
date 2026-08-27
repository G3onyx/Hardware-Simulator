cmake_minimum_required(VERSION 3.31)
cmake_policy(SET CMP0135 NEW)
project(HardwareSimulator)

set(CMAKE_CXX_STANDARD 20)

find_package(Java REQUIRED)
find_package(OpenGL REQUIRED)

include(FetchContent)

# Fetch and configure ANTLR
FetchContent_Declare(
        antlr4_runtime
        GIT_REPOSITORY https://github.com/antlr/antlr4.git
        GIT_TAG        4.13.2
        SOURCE_SUBDIR  runtime/Cpp
)
set(ANTLR_BUILD_CPP_TESTS OFF CACHE BOOL "Disable ANTLR Tests" FORCE)
set(WITH_DEMO OFF CACHE BOOL "Disable ANTLR Demo" FORCE)
FetchContent_MakeAvailable(antlr4_runtime)

# Fetch and configure OpenGL
FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4
)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "Disable GLFW Tests" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Disable GLFW Examples" FORCE)
FetchContent_MakeAvailable(glfw)

set(ANTLR_JAR "${CMAKE_CURRENT_SOURCE_DIR}/antlr/antlr-4.13.2-complete.jar")
set(GRAMMAR_FILE "${CMAKE_CURRENT_SOURCE_DIR}/antlr/HDL.g4")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(SRC_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Utils.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Blueprint.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/HDLToBlueprint.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/CompilerGraph.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Builder.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Transpiler.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/AIG.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Optimiser.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ROMLoader.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptEngine.h"
)

set(ANTLR_GENERATED_SRC
        "${GENERATED_DIR}/HDLLexer.cpp"
        "${GENERATED_DIR}/HDLParser.cpp"
        "${GENERATED_DIR}/HDLBaseVisitor.cpp"
        "${GENERATED_DIR}/HDLVisitor.cpp"
)
set(ANTLR_GENERATED_HEADERS
        "${GENERATED_DIR}/HDLLexer.h"
        "${GENERATED_DIR}/HDLParser.h"
        "${GENERATED_DIR}/HDLBaseVisitor.h"
        "${GENERATED_DIR}/HDLVisitor.h"
)

add_custom_command(
        OUTPUT ${ANTLR_GENERATED_SRC} ${ANTLR_GENERATED_HEADERS}
        COMMAND ${Java_JAVA_EXECUTABLE} -jar ${ANTLR_JAR} -Dlanguage=Cpp -visitor -no-listener -o ${GENERATED_DIR} ${GRAMMAR_FILE}
        DEPENDS ${GRAMMAR_FILE}
        COMMENT "Generating ANTLR parser files..."
)


add_library(HDLParserLib STATIC ${ANTLR_GENERATED_SRC})
target_include_directories(HDLParserLib PUBLIC "${GENERATED_DIR}")
target_link_libraries(HDLParserLib PUBLIC antlr4_static)
target_compile_definitions(HDLParserLib PUBLIC ANTLR4CPP_STATIC)

# SimCore
add_library(SimCore INTERFACE ${SRC_FILES})
target_include_directories(SimCore INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(SimCore INTERFACE HDLParserLib)
target_compile_options(SimCore INTERFACE -O3 -march=native)

# Transpiler
add_executable(Transpiler src/transpiler_main.cpp)
target_link_libraries(Transpiler PRIVATE SimCore)
target_compile_options(Transpiler PRIVATE -O3 -march=native)

add_executable(HardwareSimulator src/main.cpp testing/AutoTester.cpp testing/Tester.h testing/AutoTester.h)
target_include_directories(HardwareSimulator PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Simulator
target_link_libraries(HardwareSimulator PRIVATE SimCore glfw OpenGL::GL)
target_compile_options(HardwareSimulator PRIVATE -O3 -march=native)

add_dependencies(HardwareSimulator Transpiler)