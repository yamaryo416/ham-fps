cmake_minimum_required(VERSION 3.16)
project(hamfps_native CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

find_package(glfw3 3.3 REQUIRED)
find_package(OpenGL REQUIRED)

add_executable(hamfps main.cpp)
target_link_libraries(hamfps glfw OpenGL::GL)

if(APPLE)
  # macOSのOpenGL非推奨警告を抑制(2.1互換プロファイルは全Macで動作します)
  target_compile_definitions(hamfps PRIVATE GL_SILENCE_DEPRECATION)
endif()
