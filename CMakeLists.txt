cmake_minimum_required(VERSION 3.28)

project(json2cpp VERSION 4.8 LANGUAGES CXX)

include(FetchContent)
include(GNUInstallDirs)
include(CTest)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)

FetchContent_Declare(json EXCLUDE_FROM_ALL GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.12.0)
FetchContent_MakeAvailable(json)

if(BUILD_TESTING)
  set(VALIJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(valijson EXCLUDE_FROM_ALL GIT_REPOSITORY https://github.com/tristanpenman/valijson.git GIT_TAG v1.1.3)
  FetchContent_MakeAvailable(valijson)
endif()

add_executable(json2cpp main.cpp json2cpp.cpp)
target_compile_features(json2cpp PRIVATE cxx_std_23)
target_compile_definitions(json2cpp PRIVATE JSON2CPP_VERSION="${PROJECT_VERSION}")
target_include_directories(json2cpp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(json2cpp PRIVATE nlohmann_json::nlohmann_json)

add_library(json2cpp_headers INTERFACE)
add_library(json2cpp::headers ALIAS json2cpp_headers)
target_compile_features(json2cpp_headers INTERFACE cxx_std_23)
target_include_directories(json2cpp_headers INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

set(json2cpp_compiled_targets json2cpp)
set(json2cpp_test_targets)

if(BUILD_TESTING)
  foreach(test_name IN ITEMS literal_lookup_test literal_lookup_utf16_test)
    add_executable(${test_name} literal_lookup_test.cpp)
    target_compile_features(${test_name} PRIVATE cxx_std_23)
    if(test_name STREQUAL literal_lookup_utf16_test)
      target_compile_definitions(${test_name} PRIVATE JSON2CPP_USE_UTF16)
    endif()
    target_link_libraries(${test_name} PRIVATE json2cpp::headers)
    add_test(NAME ${test_name} COMMAND ${test_name})
    list(APPEND json2cpp_compiled_targets ${test_name})
    list(APPEND json2cpp_test_targets ${test_name})
  endforeach()

  add_executable(valijson_adapter_test valijson_adapter_test.cpp)
  target_compile_features(valijson_adapter_test PRIVATE cxx_std_23)
  target_include_directories(valijson_adapter_test SYSTEM PRIVATE "${valijson_SOURCE_DIR}/include")
  target_link_libraries(valijson_adapter_test PRIVATE json2cpp::headers)
  add_test(NAME valijson_adapter_test COMMAND valijson_adapter_test)
  list(APPEND json2cpp_compiled_targets valijson_adapter_test)
  list(APPEND json2cpp_test_targets valijson_adapter_test)

  set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
  set(generated_sources
    "${generated_dir}/allLanguages.cpp"
    "${generated_dir}/labels.cpp"
    "${generated_dir}/shadows.cpp")
  set(generated_headers
    "${generated_dir}/allLanguages.hpp" "${generated_dir}/allLanguages_impl.hpp"
    "${generated_dir}/labels.hpp" "${generated_dir}/labels_impl.hpp"
    "${generated_dir}/shadows.hpp" "${generated_dir}/shadows_impl.hpp")
  add_custom_command(
    OUTPUT ${generated_sources} ${generated_headers}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${generated_dir}"
    COMMAND $<TARGET_FILE:json2cpp> allLanguages "${CMAKE_CURRENT_SOURCE_DIR}/allLanguages.json" "${generated_dir}/allLanguages"
    COMMAND $<TARGET_FILE:json2cpp> labels "${CMAKE_CURRENT_SOURCE_DIR}/labels.json" "${generated_dir}/labels"
    COMMAND $<TARGET_FILE:json2cpp> shadows "${CMAKE_CURRENT_SOURCE_DIR}/shadows.json" "${generated_dir}/shadows"
    DEPENDS json2cpp allLanguages.json labels.json shadows.json
    VERBATIM)
  add_executable(generated_document_test generated_document_test.cpp ${generated_sources} ${generated_headers})
  target_compile_features(generated_document_test PRIVATE cxx_std_23)
  target_include_directories(generated_document_test PRIVATE "${generated_dir}")
  target_link_libraries(generated_document_test PRIVATE json2cpp::headers)
  add_test(NAME generated_document_test COMMAND generated_document_test)
  list(APPEND json2cpp_compiled_targets generated_document_test)
  list(APPEND json2cpp_test_targets generated_document_test)
endif()

foreach(target IN LISTS json2cpp_compiled_targets)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
  endif()
endforeach()

foreach(target IN LISTS json2cpp_test_targets)
  if(MSVC)
    target_compile_options(${target} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${target} PRIVATE -UNDEBUG)
  endif()
endforeach()

install(TARGETS json2cpp json2cpp_headers)
install(FILES json2cpp.hpp json2cpp_adapter.hpp DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/json2cpp")
