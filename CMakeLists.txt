cmake_minimum_required(VERSION 3.5)

project(cliupnp
        VERSION 1.0.0
        DESCRIPTION "A command-line tool for opening up UPnP ports."
        HOMEPAGE_URL "https://www.gitlab.com/cculianu/cliupnp"
        LANGUAGES CXX
)

# Package information
set(PACKAGE_NAME ${PROJECT_NAME})
set(PACKAGE_VERSION ${PROJECT_VERSION})
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(PACKAGE_NAME="${PACKAGE_NAME}" PACKAGE_VERSION="${PACKAGE_VERSION}")

if(WIN32)
    add_compile_definitions(WINDOWS=1)
elseif(UNIX)
    add_compile_definitions(UNIX=1)
endif()

add_executable(cliupnp src/main.cpp src/threadinterrupt.cpp src/upnpmgr.cpp src/util.cpp)

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
