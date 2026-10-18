find_package(ZLIB REQUIRED)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

# RPX loading is shared with the runtime so both place the image identically.
add_library(cafe_rpx STATIC src/rpx.cpp)
target_include_directories(cafe_rpx PUBLIC src ${CMAKE_SOURCE_DIR}/runtime/include)
target_compile_features(cafe_rpx PUBLIC cxx_std_20)
target_link_libraries(cafe_rpx PUBLIC ZLIB::ZLIB)
ttt2_warnings(cafe_rpx)

# Decoder is also used by the zero-install dynamic Windows host.
add_library(cafe_ppc_decode STATIC src/ppc.cpp)
target_include_directories(cafe_ppc_decode PUBLIC src)
target_compile_features(cafe_ppc_decode PUBLIC cxx_std_20)
ttt2_warnings(cafe_ppc_decode)

add_library(cafe_recomp STATIC
    src/function_table.cpp
    src/program.cpp
    src/codegen.cpp)
target_link_libraries(cafe_recomp PUBLIC cafe_rpx cafe_ppc_decode)
ttt2_warnings(cafe_recomp)

add_executable(cafe-census src/census.cpp)
target_link_libraries(cafe-census PRIVATE cafe_recomp)
ttt2_warnings(cafe-census)

add_executable(cafe-recomp src/recompile_main.cpp)
target_link_libraries(cafe-recomp PRIVATE cafe_recomp OpenSSL::Crypto)
ttt2_warnings(cafe-recomp)

add_executable(cafe-semgen src/semgen_main.cpp)
target_link_libraries(cafe-semgen PRIVATE cafe_recomp)
ttt2_warnings(cafe-semgen)
