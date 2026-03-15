set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)

set(VCPKG_LIBRARY_LINKAGE dynamic)

# Keep libass's dependency chain self-contained:
# freetype/fribidi/harfbuzz plus freetype's compression/image deps.
if(PORT STREQUAL "freetype"
   OR PORT STREQUAL "fribidi"
   OR PORT STREQUAL "harfbuzz"
   OR PORT STREQUAL "brotli"   # freetype
   OR PORT STREQUAL "bzip2"    # freetype
   OR PORT STREQUAL "libpng"   # freetype
   OR PORT STREQUAL "zlib")    # freetype, aegisub
    set(VCPKG_LIBRARY_LINKAGE static)
endif()