find_package(Git REQUIRED)
set(_tracy_dir "${CMAKE_CURRENT_LIST_DIR}/../thirdparty/tracy")
set(_tracy_patch "${CMAKE_CURRENT_LIST_DIR}/../patches/tracy-windows-sampling.patch")
if(NOT EXISTS "${_tracy_dir}/public/client/TracyProfiler.cpp")
    message(FATAL_ERROR "Missing dependencies. Run: git submodule update --init --recursive")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${_tracy_patch}"
    WORKING_DIRECTORY "${_tracy_dir}" RESULT_VARIABLE _already_applied OUTPUT_QUIET ERROR_QUIET)
if(NOT _already_applied EQUAL 0)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check "${_tracy_patch}"
        WORKING_DIRECTORY "${_tracy_dir}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${_tracy_patch}"
        WORKING_DIRECTORY "${_tracy_dir}" COMMAND_ERROR_IS_FATAL ANY)
endif()
