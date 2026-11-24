set(SOURCES
    MCPServer.cpp
)

ladybird_lib(LibMCP mcp EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibMCP PRIVATE LibCore LibHTTP LibURL)
