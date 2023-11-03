cmake_minimum_required(VERSION 3.5)

project(cliupnp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_compile_definitions(WINDOWS=1)
elseif(UNIX)
    add_compile_definitions(UNIX=1)
endif()

add_executable(cliupnp main.cpp util.cpp threadinterrupt.cpp upnpmgr.cpp)

# Add path for custom modules
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

find_package(MiniUPnPc 1.5 REQUIRED)
target_link_libraries(cliupnp MiniUPnPc::miniupnpc)

if(${CMAKE_SYSTEM_NAME} MATCHES "Windows")
  find_library(IPHLPAPI_LIBRARY NAMES iphlpapi)
  if(NOT IPHLPAPI_LIBRARY)
    message(FATAL_ERROR "Lib iphlpapi is missing")
  endif()
  target_link_libraries(cliupnp ${IPHLPAPI_LIBRARY})

  target_compile_definitions(cliupnp
    PUBLIC -DSTATICLIB
    PUBLIC -DMINIUPNP_STATICLIB
  )
endif()


include(GNUInstallDirs)
install(TARGETS cliupnp
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
