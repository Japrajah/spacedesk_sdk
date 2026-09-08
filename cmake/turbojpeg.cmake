include(ExternalProject)
set(SD_TJ_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libjpeg-turbo-3.2.0")
set(SD_TJ_BINARY "${CMAKE_CURRENT_BINARY_DIR}/turbojpeg")
get_property(SD_TJ_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(SD_TJ_MULTI_CONFIG)
  set(SD_TJ_LIBRARY "${SD_TJ_BINARY}/$<CONFIG>/turbojpeg-static.lib")
else()
  set(SD_TJ_LIBRARY "${SD_TJ_BINARY}/turbojpeg-static.lib")
endif()
find_program(SD_NASM nasm HINTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nasm-3.01" REQUIRED)
ExternalProject_Add(sd_turbojpeg_build
  SOURCE_DIR "${SD_TJ_SOURCE}"
  BINARY_DIR "${SD_TJ_BINARY}"
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}"
  CMAKE_ARGS -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=ON
    -DWITH_TOOLS=OFF -DWITH_TESTS=OFF -DWITH_CRT_DLL=OFF -DWITH_SIMD=ON
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DCMAKE_VS_GLOBALS=VcpkgEnabled=false"
    "-DCMAKE_ASM_NASM_COMPILER=${SD_NASM}"
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config $<CONFIG> --target turbojpeg-static
  INSTALL_COMMAND ""
  BUILD_ALWAYS TRUE
  BUILD_BYPRODUCTS "${SD_TJ_LIBRARY}")
add_library(sd_turbojpeg STATIC IMPORTED GLOBAL)
set_target_properties(sd_turbojpeg PROPERTIES
  IMPORTED_LOCATION "${SD_TJ_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${SD_TJ_SOURCE}/src")
if(SD_TJ_MULTI_CONFIG)
  foreach(SD_TJ_CONFIG Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${SD_TJ_CONFIG}" SD_TJ_CONFIG_UPPER)
    set_property(TARGET sd_turbojpeg PROPERTY "IMPORTED_LOCATION_${SD_TJ_CONFIG_UPPER}"
      "${SD_TJ_BINARY}/${SD_TJ_CONFIG}/turbojpeg-static.lib")
  endforeach()
endif()
add_dependencies(sd_turbojpeg sd_turbojpeg_build)
