cmake_minimum_required(VERSION 3.16)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")

set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}" CACHE INTERNAL "")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(NekoDB)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EXECUTABLE_NAME ${PROJECT_NAME})

add_compile_options(-O3 -march=native  -fno-plt)
add_compile_options(-fno-stack-protector -fomit-frame-pointer -flto=full)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-mllvm -inline-threshold=5000)
    add_link_options(-static) 
    add_link_options(-static-libgcc -static-libstdc++) 
    
endif()

if (ANDROID)
    set(EXECUTABLE_NAME main)
    add_library(${EXECUTABLE_NAME} SHARED)
else()
    add_executable(${EXECUTABLE_NAME})
endif()

target_sources(${EXECUTABLE_NAME}
PRIVATE 
   
    Engine/main.cpp
    Engine/Neko_Input.cpp
    Engine/glad.c
)
target_compile_definitions(${EXECUTABLE_NAME} PRIVATE BASISD_SUPPORT_KTX2_ZSTD=1)

set(CMAKE_C_FLAGS  "${CMAKE_C_FLAGS} -DBASISU_SUPPORT_SSE=1 -msse4.1")
set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -DBASISU_SUPPORT_SSE=1 -msse4.1 -avx")


target_compile_features(${EXECUTABLE_NAME} PUBLIC cxx_std_23)
set(CMAKE_CXX_FLAGS -std=c++23)

set(SDL3_DIR "NekoAddons/SDL/build")

#find_package(SDL3 REQUIRED)

option(NEKO_DYNAMIC_LIBRARY "Build NekoEngine2 as a dynamic library" OFF)
option(NEKO_PIC "Build NekoEngine2 as position-independent code" NEKO_DYNAMIC_LIBRARY)




target_compile_options(${EXECUTABLE_NAME} PRIVATE -falign-loops=32)

find_package(OpenMP REQUIRED)


target_include_directories(${EXECUTABLE_NAME} PUBLIC
${PROJECT_SOURCE_ROOT}/src
${PROJECT_SOURCE_ROOT}


${SDL3_INCLUDE_DIRS}
)

add_subdirectory(NekoAddons/SDL EXCLUDE_FROM_ALL)

set(ADDITIONAL_LIBRARY_DEPENDENCIES
    vulkan
    #glm
)

target_link_libraries(${EXECUTABLE_NAME} PUBLIC
   SDL3::SDL3;
   ws2_32     
   mswsock     
   advapi32    

OpenMP::OpenMP_CXX
   #"${ADDITIONAL_LIBRARY_DEPENDENCIES}"
)

target_compile_definitions(${PROJECT_NAME} PRIVATE 
    
    WIN32_LEAN_AND_MEAN
)