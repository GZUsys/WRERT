set(ROERT_SOURCES
        roert.cpp roert.h
        roert_node.cpp roert_node.h)

add_library(nvmkv-roert STATIC ${ROERT_SOURCES})
target_link_libraries(nvmkv-roert PUBLIC nvmkv-fastalloc)
