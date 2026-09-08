# Checked-in fallback keeps the build usable without a separate shader tool.
find_program(SD_FXC fxc HINTS
  "$ENV{WindowsSdkVerBinPath}/x64"
  "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
if(SD_FXC AND Python3_FOUND)
  set(SD_SHADER_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")
  set(SD_SHADER_HEADER "${SD_SHADER_INCLUDE_DIR}/sdk_shaders.h")
  add_custom_command(OUTPUT "${SD_SHADER_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_shaders.py"
      --fxc "${SD_FXC}" --output "${SD_SHADER_HEADER}"
    DEPENDS tools/compile_shaders.py sdk/shaders/vertex.hlsl sdk/shaders/pixel.hlsl
    VERBATIM)
else()
  set(SD_SHADER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk/shaders/prebuilt")
  set(SD_SHADER_HEADER "${SD_SHADER_INCLUDE_DIR}/sdk_shaders.h")
endif()
