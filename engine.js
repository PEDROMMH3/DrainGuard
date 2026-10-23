export async function loadEngine(bytes) {
 const {instance}=await WebAssembly.instantiate(bytes,{wasi_snapshot_preview1:{fd_close:()=>0,fd_seek:()=>8,fd_write:()=>8}});
 instance.exports._initialize();return instance.exports;
}
