# Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
#
# SPDX-License-Identifier: BSD-3-Clause

# Run the Poky environment script if the path is provided
# Check env var first, then cmake cache variable as fallback (used by ExternalProject subprocesses)
set(POKY_ENV_SCRIPT "$ENV{POKY_ENVIRONMENT_SETUP}")
if(NOT POKY_ENV_SCRIPT AND DEFINED POKY_ENVIRONMENT_SETUP)
  set(POKY_ENV_SCRIPT "${POKY_ENVIRONMENT_SETUP}")
endif()
if(POKY_ENV_SCRIPT)
  if(NOT EXISTS "${POKY_ENV_SCRIPT}")
    message(FATAL_ERROR "Poky environment script not found: ${POKY_ENV_SCRIPT}")
  endif()
  execute_process(
    COMMAND /bin/bash -c "unset LD_LIBRARY_PATH; source '${POKY_ENV_SCRIPT}' >/dev/null 2>&1; env"
    OUTPUT_VARIABLE ENV_VARS
  )
  string(REGEX REPLACE "\n" ";" ENV_VARS_LIST "${ENV_VARS}")
  foreach(VAR_DEF ${ENV_VARS_LIST})
    string(REGEX MATCH "([^=]+)=(.*)" _ ${VAR_DEF})
    set(VAR_NAME ${CMAKE_MATCH_1})
    set(VAR_VALUE ${CMAKE_MATCH_2})
    set(ENV{${VAR_NAME}} "${VAR_VALUE}")
  endforeach()
  message(STATUS "Loaded Poky environment from ${POKY_ENV_SCRIPT}")
else()
  message(STATUS "POKY_ENVIRONMENT_SETUP not provided; assuming Poky variables are already in the environment")
endif()

# Debug prints to check environment variables
message(STATUS "CC: $ENV{CC}")
message(STATUS "CXX: $ENV{CXX}")

foreach(_required_var CC CXX SDKTARGETSYSROOT)
  if(NOT DEFINED ENV{${_required_var}})
    message(FATAL_ERROR "Required Poky environment variable ${_required_var} is not defined. Source the Poky setup script or set POKY_ENVIRONMENT_SETUP.")
  endif()
endforeach()

# Function to extract compiler and flags
function(extract_compiler_and_flags ENV_VAR COMPILER_VAR FLAGS_VAR)
  # Get the environment variable
  set(COMPILER_AND_FLAGS "$ENV{${ENV_VAR}}")

  # Debug print to check the value of the environment variable
  message(STATUS "${ENV_VAR}: ${COMPILER_AND_FLAGS}")

  # Use string separation to extract compiler and flags
  separate_arguments(COMPILER_AND_FLAGS_LIST UNIX_COMMAND "${COMPILER_AND_FLAGS}")
  list(GET COMPILER_AND_FLAGS_LIST 0 COMPILER_EXEC)
  list(REMOVE_AT COMPILER_AND_FLAGS_LIST 0)
  string(REPLACE ";" " " COMPILER_FLAGS "${COMPILER_AND_FLAGS_LIST}")

  # Set the output variables
  set(${COMPILER_VAR} "${COMPILER_EXEC}" PARENT_SCOPE)
  set(${FLAGS_VAR} "${COMPILER_FLAGS}" PARENT_SCOPE)
endfunction()

# Extract CC and its flags
extract_compiler_and_flags("CC" ENV_CC_COMPILER ENV_CC_FLAGS)

# Extract CXX and its flags
extract_compiler_and_flags("CXX" ENV_CXX_COMPILER ENV_CXX_FLAGS)

# Set compilers and sysroot
set(CMAKE_C_COMPILER "${ENV_CC_COMPILER}")
set(CMAKE_CXX_COMPILER "${ENV_CXX_COMPILER}")
set(CMAKE_SYSROOT "$ENV{SDKTARGETSYSROOT}")

# Prefer the Micro-XRCE-DDS-Agent temporary install tree before falling back to host paths
get_filename_component(CROSS_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
set(MICRO_XRCE_AGENT_DIR "${CROSS_DIR}/Micro-XRCE-DDS-Agent")
set(MICRO_XRCE_BUILD_DIR "${MICRO_XRCE_AGENT_DIR}/build")
set(MICRO_XRCE_TEMP_INSTALL_DIR "${MICRO_XRCE_BUILD_DIR}/temp_install")
list(APPEND CMAKE_PREFIX_PATH "${MICRO_XRCE_TEMP_INSTALL_DIR}")
if(EXISTS "${MICRO_XRCE_TEMP_INSTALL_DIR}")
  file(GLOB MICRO_XRCE_DEPENDENCY_DIRS "${MICRO_XRCE_TEMP_INSTALL_DIR}/*")
  foreach(_dep_dir ${MICRO_XRCE_DEPENDENCY_DIRS})
    if(IS_DIRECTORY "${_dep_dir}")
      list(APPEND CMAKE_PREFIX_PATH "${_dep_dir}")
    endif()
  endforeach()
endif()

# Restrict package/library/include lookups to the Poky sysroot and temp install tree
set(_FIND_ROOT_PATHS "$ENV{SDKTARGETSYSROOT}" "${CMAKE_SYSROOT}" "${MICRO_XRCE_TEMP_INSTALL_DIR}")
list(REMOVE_DUPLICATES _FIND_ROOT_PATHS)
set(CMAKE_FIND_ROOT_PATH "${_FIND_ROOT_PATHS}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Set compilation and linker flags
set(CMAKE_C_FLAGS_INIT "${ENV_CC_FLAGS} $ENV{CFLAGS} $ENV{CPPFLAGS} -Wno-poison-system-directories")
set(CMAKE_CXX_FLAGS_INIT "${ENV_CXX_FLAGS} $ENV{CXXFLAGS} $ENV{CPPFLAGS} -Wno-maybe-uninitialized -Wno-poison-system-directories")
set(CMAKE_EXE_LINKER_FLAGS_INIT "$ENV{LDFLAGS}")

# Include directories and library directories from the sysroot
# Poky SDK uses lib64; Docker/Ubuntu sysroot uses lib/aarch64-linux-gnu
include_directories("$ENV{SDKTARGETSYSROOT}/usr/include")
link_directories("$ENV{SDKTARGETSYSROOT}/usr/lib64")
link_directories("$ENV{SDKTARGETSYSROOT}/usr/lib/aarch64-linux-gnu")
