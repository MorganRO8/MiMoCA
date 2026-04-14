cmake_minimum_required(VERSION 3.20)
project(MiMoCA LANGUAGES CXX)

include(GNUInstallDirs)
include(CheckLibraryExists)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_executable(mimoca src/main.cpp)
target_compile_features(mimoca PRIVATE cxx_std_20)

find_package(OpenCV QUIET COMPONENTS core videoio imgproc imgcodecs)
if(OpenCV_FOUND)
  target_compile_definitions(mimoca PRIVATE MIMOCA_HAS_OPENCV=1)
  target_link_libraries(mimoca PRIVATE ${OpenCV_LIBS})
endif()

if(WIN32)
  target_compile_definitions(mimoca PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  set(MIMOCA_HAS_WASAPI 1)
  target_link_libraries(mimoca PRIVATE Mmdevapi Audioclient)
  #set(MIMOCA_HAS_WASAPI 0)
  #check_library_exists(Audioclient AUDCLNT_E_ENDPOINT_CREATE_FAILED "" MIMOCA_HAS_AUDIOCLIENT_LIB)
  #check_library_exists(Mmdevapi DllGetClassObject "" MIMOCA_HAS_MMDEVAPI_LIB)
  #if(MIMOCA_HAS_AUDIOCLIENT_LIB AND MIMOCA_HAS_MMDEVAPI_LIB)
    #set(MIMOCA_HAS_WASAPI 1)
    #target_link_libraries(mimoca PRIVATE Mmdevapi Audioclient)
  #else()
    #message(WARNING "WASAPI libraries not found in the active Windows SDK; building without live microphone capture.")
  #endif()
  target_compile_definitions(mimoca PRIVATE MIMOCA_HAS_WASAPI=${MIMOCA_HAS_WASAPI})
  option(MIMOCA_ENABLE_QT_UI "Enable Qt-based primary UI layer" ON)
  if(MIMOCA_ENABLE_QT_UI)
    find_package(Qt6 QUIET COMPONENTS Core Gui Widgets)
    if(Qt6_FOUND)
      target_compile_definitions(mimoca PRIVATE MIMOCA_HAS_QT=1)
      target_link_libraries(mimoca PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
      set_target_properties(mimoca PROPERTIES WIN32_EXECUTABLE TRUE)
    else()
      message(WARNING "Qt6 not found; building without Qt UI layer.")
    endif()
  endif()
endif()

if(MSVC)
  target_compile_definitions(mimoca PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(mimoca PRIVATE /W4 /permissive-)
  target_link_libraries(mimoca PRIVATE sapi ole32 ws2_32 advapi32 crypt32)
else()
  target_compile_options(mimoca PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(MIMOCA_RELEASE_LAYOUT_ROOT "MiMoCA")
set(MIMOCA_MANAGED_PYTHON_RUNTIME "" CACHE PATH "Path to a managed Python runtime to package under runtime/python")

install(TARGETS mimoca RUNTIME DESTINATION ${MIMOCA_RELEASE_LAYOUT_ROOT}/bin)
install(DIRECTORY assets/ DESTINATION ${MIMOCA_RELEASE_LAYOUT_ROOT}/assets)
install(DIRECTORY python/ DESTINATION ${MIMOCA_RELEASE_LAYOUT_ROOT}/python)
install(DIRECTORY scripts/ DESTINATION ${MIMOCA_RELEASE_LAYOUT_ROOT}/scripts)

if(MIMOCA_MANAGED_PYTHON_RUNTIME)
  install(DIRECTORY "${MIMOCA_MANAGED_PYTHON_RUNTIME}/" DESTINATION ${MIMOCA_RELEASE_LAYOUT_ROOT}/runtime/python)
endif()

set(MIMOCA_READINESS_CHECKLIST "${CMAKE_SOURCE_DIR}/scripts/release_checklist.md" CACHE FILEPATH "Release readiness checklist path")
set(MIMOCA_SMOKE_REPORT "${CMAKE_SOURCE_DIR}/artifacts/readiness/smoke_report.json" CACHE FILEPATH "Release smoke report path")
set(MIMOCA_MANUAL_QA_REPORT "${CMAKE_SOURCE_DIR}/artifacts/readiness/manual_qa_report.json" CACHE FILEPATH "Release manual QA report path")

add_custom_target(mimoca_release_smoke
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/smoke_release.py --repo ${CMAKE_SOURCE_DIR} --report ${MIMOCA_SMOKE_REPORT}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Running MiMoCA release smoke checks")

add_custom_target(mimoca_release_readiness
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/release_gate.py
          --checklist ${MIMOCA_READINESS_CHECKLIST}
          --smoke-report ${MIMOCA_SMOKE_REPORT}
          --manual-report ${MIMOCA_MANUAL_QA_REPORT}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Validating MiMoCA release readiness gate")
add_dependencies(mimoca_release_readiness mimoca_release_smoke)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_dependencies(mimoca mimoca_release_readiness)
endif()
