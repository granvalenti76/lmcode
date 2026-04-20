set(TARGET llama-cli)
add_executable(${TARGET}
    cli.cpp
    cli-tool.cpp
    cli-tool-exec.cpp
    cli-tool-parser.cpp
    cli-stats.cpp
    cli_tui.cpp
    output_buffer.cpp
)
target_link_libraries(${TARGET} PRIVATE server-context llama-common llama ggml ${CMAKE_THREAD_LIBS_INIT})
target_compile_features(${TARGET} PRIVATE cxx_std_17)

include_directories(../server)

if(LLAMA_TOOLS_INSTALL)
    install(TARGETS ${TARGET} RUNTIME)
endif()
