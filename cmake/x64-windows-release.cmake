set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)

set(VCPKG_LIBRARY_LINKAGE dynamic)

# Keep libass's dependency chain self-contained:
# freetype/fribidi/harfbuzz plus freetype's compression/image deps.
if(PORT STREQUAL "freetype"
   OR PORT STREQUAL "fribidi"
   OR PORT STREQUAL "harfbuzz"
   OR PORT STREQUAL "brotli"
   OR PORT STREQUAL "bzip2"
   OR PORT STREQUAL "libpng"
   OR PORT STREQUAL "zlib")
    set(VCPKG_LIBRARY_LINKAGE static)
endif()

# Build only Release-configuration binaries to keep the install tree lean.
set(VCPKG_BUILD_TYPE release)

