set(WRERT_SOURCES
        wrert.cpp wrert.h
        wrert_node.cpp wrert_node.h)

add_library(nvmkv-wrert STATIC ${WRERT_SOURCES})
target_link_libraries(nvmkv-wrert PUBLIC nvmkv-fastalloc)
