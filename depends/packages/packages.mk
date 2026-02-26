equihash_packages := libsodium
packages:=boost openssl libevent zeromq $(equihash_packages)

protobuf_native_packages = native_protobuf
protobuf_packages = protobuf

openssl_packages = openssl

qrencode_packages = qrencode

qt_native_packages = native_protobuf
qt_packages = qrencode protobuf zlib

qt_linux_packages:=qt expat libxcb xcb_proto libXau xproto freetype fontconfig libxkbcommon libxcb_util libxcb_util_render libxcb_util_keysyms libxcb_util_image libxcb_util_wm

qt_darwin_packages=qt
qt_mingw32_packages=qt zlib

bdb_packages=bdb
#sqlite_packages=sqlite

zmq_packages=zeromq

upnp_packages=miniupnpc
# natpmp_packages=libnatpmp

# jemalloc_packages = jemalloc

#ifeq ($(host_os),linux)
#cpython_native_packages = native_cpython
#cpython_packages = cpython
#endif
