add_library(networking INTERFACE)

target_sources(networking INTERFACE
 ${CMAKE_CURRENT_LIST_DIR}/base64.c
 ${CMAKE_CURRENT_LIST_DIR}/cJSON.c
 ${CMAKE_CURRENT_LIST_DIR}/fs_ram.c
 ${CMAKE_CURRENT_LIST_DIR}/fs_stream.c
 ${CMAKE_CURRENT_LIST_DIR}/ftpd.c
 ${CMAKE_CURRENT_LIST_DIR}/http_upload.c
 ${CMAKE_CURRENT_LIST_DIR}/httpd.c
 ${CMAKE_CURRENT_LIST_DIR}/multipartparser.c
 ${CMAKE_CURRENT_LIST_DIR}/networking.c
 ${CMAKE_CURRENT_LIST_DIR}/sfifo.c
 ${CMAKE_CURRENT_LIST_DIR}/sha1.c
 ${CMAKE_CURRENT_LIST_DIR}/strutils.c
 ${CMAKE_CURRENT_LIST_DIR}/telnetd.c
 ${CMAKE_CURRENT_LIST_DIR}/urldecode.c
 ${CMAKE_CURRENT_LIST_DIR}/urlencode.c
 ${CMAKE_CURRENT_LIST_DIR}/utils.c
 ${CMAKE_CURRENT_LIST_DIR}/webdav.c
 ${CMAKE_CURRENT_LIST_DIR}/websocketd.c
 ${CMAKE_CURRENT_LIST_DIR}/ssdp.c
 ${CMAKE_CURRENT_LIST_DIR}/mqtt.c
 ${CMAKE_CURRENT_LIST_DIR}/modbus/client.c
 )

target_include_directories(networking INTERFACE ${CMAKE_CURRENT_LIST_DIR})
