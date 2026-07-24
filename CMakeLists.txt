cmake_minimum_required(VERSION 3.10)
project(p2p_punch C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common sources
set(COMMON_SOURCES utils.c xpoll.c iocp.c zcpool.c)

# Server
add_executable(p2p_server
    main_server.c
    server.c
    ${COMMON_SOURCES}
)
target_link_libraries(p2p_server PRIVATE)

# Client
add_executable(p2p_client
    main_client.c
    client.c
    ${COMMON_SOURCES}
)
target_link_libraries(p2p_client PRIVATE)

if(UNIX AND NOT APPLE)
    target_link_libraries(p2p_server PRIVATE rt)
    target_link_libraries(p2p_client PRIVATE rt)
    target_compile_definitions(p2p_server PRIVATE _GNU_SOURCE)
    target_compile_definitions(p2p_client PRIVATE _GNU_SOURCE)
endif()

if(WIN32)
    target_link_libraries(p2p_server PRIVATE ws2_32)
    target_link_libraries(p2p_client PRIVATE ws2_32 iphlpapi)
    target_compile_definitions(p2p_server PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(p2p_client PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()


install(TARGETS p2p_server p2p_client DESTINATION bin)
