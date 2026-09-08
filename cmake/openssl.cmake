# Pinned, offline, static OpenSSL; separate configuration trees keep CRTs apart.
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The SDK OpenSSL build currently requires MSVC x64.")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SD_OPENSSL_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssl-3.5.8")
set(SD_OPENSSL_BINARY "${CMAKE_CURRENT_BINARY_DIR}/openssl")
set(SD_OPENSSL_PERL "${CMAKE_CURRENT_SOURCE_DIR}/third_party/strawberry-perl-5.42.3.1/perl/bin/perl.exe"
  CACHE FILEPATH "Native Windows Perl used only to build OpenSSL")
find_program(SD_OPENSSL_NASM nasm HINTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nasm-3.01" REQUIRED)
set(SD_OPENSSL_VS_INSTANCE "${CMAKE_GENERATOR_INSTANCE}" CACHE PATH "Visual Studio installation used for OpenSSL")
if(NOT SD_OPENSSL_VS_INSTANCE)
  message(FATAL_ERROR "Set SD_OPENSSL_VS_INSTANCE to the Visual Studio installation directory.")
endif()
foreach(SD_OPENSSL_CONFIG Debug Release RelWithDebInfo MinSizeRel)
  file(MAKE_DIRECTORY "${SD_OPENSSL_BINARY}/${SD_OPENSSL_CONFIG}/include")
endforeach()
add_custom_target(sd_openssl_build
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_openssl.py"
    --source "${SD_OPENSSL_SOURCE}" --build "${SD_OPENSSL_BINARY}" --config "$<CONFIG>"
    --vs-instance "${SD_OPENSSL_VS_INSTANCE}" --perl "${SD_OPENSSL_PERL}" --nasm "${SD_OPENSSL_NASM}"
  BYPRODUCTS "${SD_OPENSSL_BINARY}/$<CONFIG>/libssl.lib" "${SD_OPENSSL_BINARY}/$<CONFIG>/libcrypto.lib"
  VERBATIM)
foreach(SD_OPENSSL_LIBRARY ssl crypto)
  add_library(sd_openssl_${SD_OPENSSL_LIBRARY} STATIC IMPORTED GLOBAL)
  set_target_properties(sd_openssl_${SD_OPENSSL_LIBRARY} PROPERTIES
    IMPORTED_LOCATION "${SD_OPENSSL_BINARY}/$<CONFIG>/lib${SD_OPENSSL_LIBRARY}.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${SD_OPENSSL_BINARY}/$<CONFIG>/include;${SD_OPENSSL_SOURCE}/include")
  foreach(SD_OPENSSL_CONFIG Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${SD_OPENSSL_CONFIG}" SD_OPENSSL_CONFIG_UPPER)
    set_property(TARGET sd_openssl_${SD_OPENSSL_LIBRARY}
      PROPERTY "IMPORTED_LOCATION_${SD_OPENSSL_CONFIG_UPPER}"
      "${SD_OPENSSL_BINARY}/${SD_OPENSSL_CONFIG}/lib${SD_OPENSSL_LIBRARY}.lib")
  endforeach()
  add_dependencies(sd_openssl_${SD_OPENSSL_LIBRARY} sd_openssl_build)
endforeach()
add_library(sd_openssl INTERFACE)
target_link_libraries(sd_openssl INTERFACE sd_openssl_ssl sd_openssl_crypto
  ws2_32 gdi32 advapi32 crypt32 user32)
add_dependencies(sd_openssl sd_openssl_build)
