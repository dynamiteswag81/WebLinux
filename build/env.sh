# Shared settings for the WebLinux user space toolchain (source this).
W=/wl
SYSROOT=/c/Users/dynam/wltools/sysroot
WASM_ARCH_FLAGS="--target=wasm32-unknown-unknown -matomics -mbulk-memory -mmutable-globals -msign-ext -mnontrapping-fptoint -mexception-handling -mllvm -wasm-enable-sjlj"
export W SYSROOT WASM_ARCH_FLAGS
