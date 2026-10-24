set(SOURCES
    ladybird_port.cpp
)

ladybird_lib(LibPort port)

# LibWebView links most of its dependencies PRIVATE, so they do not propagate to a consumer that
# reaches into its headers. Mirror its link line here rather than relying on transitivity.
target_link_libraries(LibPort PRIVATE AK LibCore LibGfx LibIPC LibJS LibRequests LibURL LibWeb LibWebView)

if (APPLE)
    target_link_libraries(LibPort PRIVATE "-framework CoreFoundation")
endif()
