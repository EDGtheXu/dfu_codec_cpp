# 方案 B（泛化 ops）的阶段 0 原型：设计实验，不是库的一部分。
#
#   cmake -S . -B build-poc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCODEC_BUILD_PROTOTYPE=ON
#   cmake --build build-poc
#   build-poc/prototype/codec_generic_ops_poc.exe        # 24 字节句柄（默认）
#   build-poc/prototype/codec_generic_ops_poc_tagged.exe # 32 字节句柄（每个值带标签）
#
# 两份可执行文件的代码完全相同，只有句柄尺寸这个编译期开关不同，
# 这样才能把"多 8 字节"的代价单独量出来。

set(CODEC_POC_SOURCES poc_main.cpp)

add_executable(codec_generic_ops_poc ${CODEC_POC_SOURCES})
target_compile_definitions(codec_generic_ops_poc PRIVATE PROTO_TAGGED_HANDLE=0)

add_executable(codec_generic_ops_poc_tagged ${CODEC_POC_SOURCES})
target_compile_definitions(codec_generic_ops_poc_tagged PRIVATE PROTO_TAGGED_HANDLE=1)

foreach(poc_target codec_generic_ops_poc codec_generic_ops_poc_tagged)
  target_link_libraries(${poc_target} PRIVATE codec_json)
  if(MSVC)
    target_compile_options(${poc_target} PRIVATE /utf-8 /W4 /permissive- /bigobj)
  else()
    target_compile_options(${poc_target} PRIVATE -Wall -Wextra)
  endif()
endforeach()
