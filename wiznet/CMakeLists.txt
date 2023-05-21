add_library(wiznet INTERFACE)

target_sources(wiznet INTERFACE
 ${CMAKE_CURRENT_LIST_DIR}/enet.c
 ${CMAKE_CURRENT_LIST_DIR}/socket.c
 ${CMAKE_CURRENT_LIST_DIR}/wizchip_conf.c
 ${CMAKE_CURRENT_LIST_DIR}/port/w5x00_lwip.c
 ${CMAKE_CURRENT_LIST_DIR}/W5100S/w5100s.c
 ${CMAKE_CURRENT_LIST_DIR}/W5500/w5500.c
 ${CMAKE_CURRENT_LIST_DIR}/arch/sys_arch.c
 )

target_include_directories(wiznet INTERFACE ${CMAKE_CURRENT_LIST_DIR})
