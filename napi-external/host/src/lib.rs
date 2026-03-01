use anyhow::{Context, Result};
use std::ffi::CString;
use std::path::{Path, PathBuf};
use wasmer::{
    imports, Function, FunctionEnv, FunctionEnvMut, Imports, Instance, Memory, Module, Store,
    TypedFunction,
    sys::{EngineBuilder, Features},
};
use wasmer_cache::{Cache, FileSystemCache, Hash as CacheHash};
use wasmer_compiler_llvm::{LLVM, LLVMOptLevel};
use wasmer_wasix::{Pipe, WasiEnv, WasiError};
use virtual_fs::AsyncReadExt;
use virtual_mio::block_on;

const MAX_GUEST_CSTRING_SCAN: usize = 64 * 1024;

// ============================================================
// C++ bridge FFI declarations (from napi_bridge_init.cc)
// ============================================================
unsafe extern "C" {
    fn snapi_bridge_init() -> i32;
    // Value creation
    fn snapi_bridge_get_undefined(out_id: *mut u32) -> i32;
    fn snapi_bridge_get_null(out_id: *mut u32) -> i32;
    fn snapi_bridge_get_boolean(value: i32, out_id: *mut u32) -> i32;
    fn snapi_bridge_get_global(out_id: *mut u32) -> i32;
    fn snapi_bridge_create_string_utf8(str_ptr: *const i8, wasm_length: u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_create_string_latin1(str_ptr: *const i8, wasm_length: u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_create_int32(value: i32, out_id: *mut u32) -> i32;
    fn snapi_bridge_create_uint32(value: u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_create_double(value: f64, out_id: *mut u32) -> i32;
    fn snapi_bridge_create_int64(value: i64, out_id: *mut u32) -> i32;
    fn snapi_bridge_create_object(out_id: *mut u32) -> i32;
    fn snapi_bridge_create_array(out_id: *mut u32) -> i32;
    fn snapi_bridge_create_array_with_length(length: u32, out_id: *mut u32) -> i32;
    // Value reading
    fn snapi_bridge_get_value_string_utf8(id: u32, buf: *mut i8, bufsize: usize, result: *mut usize) -> i32;
    fn snapi_bridge_get_value_string_latin1(id: u32, buf: *mut i8, bufsize: usize, result: *mut usize) -> i32;
    fn snapi_bridge_get_value_int32(id: u32, result: *mut i32) -> i32;
    fn snapi_bridge_get_value_uint32(id: u32, result: *mut u32) -> i32;
    fn snapi_bridge_get_value_double(id: u32, result: *mut f64) -> i32;
    fn snapi_bridge_get_value_int64(id: u32, result: *mut i64) -> i32;
    fn snapi_bridge_get_value_bool(id: u32, result: *mut i32) -> i32;
    // Type checking
    fn snapi_bridge_typeof(id: u32, result: *mut i32) -> i32;
    fn snapi_bridge_is_array(id: u32, result: *mut i32) -> i32;
    fn snapi_bridge_is_error(id: u32, result: *mut i32) -> i32;
    fn snapi_bridge_is_arraybuffer(id: u32, result: *mut i32) -> i32;
    fn snapi_bridge_is_typedarray(id: u32, result: *mut i32) -> i32;
    fn snapi_bridge_is_dataview(id: u32, result: *mut i32) -> i32;
    fn snapi_bridge_is_date(id: u32, result: *mut i32) -> i32;
    fn snapi_bridge_is_promise(id: u32, result: *mut i32) -> i32;
    fn snapi_bridge_instanceof(obj_id: u32, ctor_id: u32, result: *mut i32) -> i32;
    // Coercion
    fn snapi_bridge_coerce_to_bool(id: u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_coerce_to_number(id: u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_coerce_to_string(id: u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_coerce_to_object(id: u32, out_id: *mut u32) -> i32;
    // Object operations
    fn snapi_bridge_set_property(obj_id: u32, key_id: u32, val_id: u32) -> i32;
    fn snapi_bridge_get_property(obj_id: u32, key_id: u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_has_property(obj_id: u32, key_id: u32, result: *mut i32) -> i32;
    fn snapi_bridge_has_own_property(obj_id: u32, key_id: u32, result: *mut i32) -> i32;
    fn snapi_bridge_delete_property(obj_id: u32, key_id: u32, result: *mut i32) -> i32;
    fn snapi_bridge_set_named_property(obj_id: u32, name: *const i8, val_id: u32) -> i32;
    fn snapi_bridge_get_named_property(obj_id: u32, name: *const i8, out_id: *mut u32) -> i32;
    fn snapi_bridge_has_named_property(obj_id: u32, name: *const i8, result: *mut i32) -> i32;
    fn snapi_bridge_set_element(obj_id: u32, index: u32, val_id: u32) -> i32;
    fn snapi_bridge_get_element(obj_id: u32, index: u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_has_element(obj_id: u32, index: u32, result: *mut i32) -> i32;
    fn snapi_bridge_delete_element(obj_id: u32, index: u32, result: *mut i32) -> i32;
    fn snapi_bridge_get_array_length(arr_id: u32, result: *mut u32) -> i32;
    fn snapi_bridge_get_property_names(obj_id: u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_get_all_property_names(obj_id: u32, mode: i32, filter: i32, conversion: i32, out_id: *mut u32) -> i32;
    fn snapi_bridge_get_prototype(obj_id: u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_object_freeze(obj_id: u32) -> i32;
    fn snapi_bridge_object_seal(obj_id: u32) -> i32;
    // Comparison
    fn snapi_bridge_strict_equals(a_id: u32, b_id: u32, result: *mut i32) -> i32;
    // Error handling
    fn snapi_bridge_create_error(code_id: u32, msg_id: u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_create_type_error(code_id: u32, msg_id: u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_create_range_error(code_id: u32, msg_id: u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_throw(error_id: u32) -> i32;
    fn snapi_bridge_throw_error(code: *const i8, msg: *const i8) -> i32;
    fn snapi_bridge_throw_type_error(code: *const i8, msg: *const i8) -> i32;
    fn snapi_bridge_throw_range_error(code: *const i8, msg: *const i8) -> i32;
    fn snapi_bridge_is_exception_pending(result: *mut i32) -> i32;
    fn snapi_bridge_get_and_clear_last_exception(out_id: *mut u32) -> i32;
    // Symbol
    fn snapi_bridge_create_symbol(description_id: u32, out_id: *mut u32) -> i32;
    // BigInt
    fn snapi_bridge_create_bigint_int64(value: i64, out_id: *mut u32) -> i32;
    fn snapi_bridge_create_bigint_uint64(value: u64, out_id: *mut u32) -> i32;
    fn snapi_bridge_get_value_bigint_int64(id: u32, value: *mut i64, lossless: *mut i32) -> i32;
    fn snapi_bridge_get_value_bigint_uint64(id: u32, value: *mut u64, lossless: *mut i32) -> i32;
    // Date
    fn snapi_bridge_create_date(time: f64, out_id: *mut u32) -> i32;
    fn snapi_bridge_get_date_value(id: u32, result: *mut f64) -> i32;
    // Promise
    fn snapi_bridge_create_promise(deferred_out: *mut u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_resolve_deferred(deferred_id: u32, value_id: u32) -> i32;
    fn snapi_bridge_reject_deferred(deferred_id: u32, value_id: u32) -> i32;
    // ArrayBuffer
    fn snapi_bridge_create_arraybuffer(byte_length: u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_create_external_arraybuffer(data_addr: u64, byte_length: u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_get_arraybuffer_info(id: u32, data_out: *mut u64, byte_length: *mut u32) -> i32;
    fn snapi_bridge_detach_arraybuffer(id: u32) -> i32;
    fn snapi_bridge_is_detached_arraybuffer(id: u32, result: *mut i32) -> i32;
    // TypedArray
    fn snapi_bridge_create_typedarray(typ: i32, length: u32, arraybuffer_id: u32, byte_offset: u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_get_typedarray_info(id: u32, type_out: *mut i32, length_out: *mut u32, arraybuffer_out: *mut u32, byte_offset_out: *mut u32) -> i32;
    // DataView
    fn snapi_bridge_create_dataview(byte_length: u32, arraybuffer_id: u32, byte_offset: u32, out_id: *mut u32) -> i32;
    fn snapi_bridge_get_dataview_info(id: u32, byte_length_out: *mut u32, arraybuffer_out: *mut u32, byte_offset_out: *mut u32) -> i32;
    // External
    fn snapi_bridge_create_external(data_val: u64, out_id: *mut u32) -> i32;
    fn snapi_bridge_get_value_external(id: u32, data_out: *mut u64) -> i32;
    // References
    fn snapi_bridge_create_reference(value_id: u32, initial_refcount: u32, ref_out: *mut u32) -> i32;
    fn snapi_bridge_delete_reference(ref_id: u32) -> i32;
    fn snapi_bridge_reference_ref(ref_id: u32, result: *mut u32) -> i32;
    fn snapi_bridge_reference_unref(ref_id: u32, result: *mut u32) -> i32;
    fn snapi_bridge_get_reference_value(ref_id: u32, out_id: *mut u32) -> i32;
    // Handle scopes (escapable)
    fn snapi_bridge_open_escapable_handle_scope(scope_out: *mut u32) -> i32;
    fn snapi_bridge_close_escapable_handle_scope(scope_id: u32) -> i32;
    fn snapi_bridge_escape_handle(scope_id: u32, escapee_id: u32, out_id: *mut u32) -> i32;
    // Type tagging
    fn snapi_bridge_type_tag_object(obj_id: u32, tag_lower: u64, tag_upper: u64) -> i32;
    fn snapi_bridge_check_object_type_tag(obj_id: u32, tag_lower: u64, tag_upper: u64, result: *mut i32) -> i32;
    // Function calling
    fn snapi_bridge_call_function(recv_id: u32, func_id: u32, argc: u32, argv_ids: *const u32, out_id: *mut u32) -> i32;
    // Script execution
    fn snapi_bridge_run_script(script_id: u32, out_value_id: *mut u32) -> i32;
    // Cleanup
    #[allow(dead_code)]
    fn snapi_bridge_dispose();
}

// ============================================================
// Runtime state shared between host and WASM guest
// ============================================================

struct RuntimeEnv {
    memory: Option<Memory>,
    malloc_fn: Option<TypedFunction<i32, i32>>,
}

fn make_store() -> Store {
    let mut features = Features::default();
    features.exceptions(true);
    let mut compiler = LLVM::default();
    compiler.opt_level(LLVMOptLevel::Less);
    let engine = EngineBuilder::new(compiler)
        .set_features(Some(features))
        .engine();
    Store::new(engine)
}

fn wasmer_cache_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap_or_else(|| Path::new("."))
        .join("target")
        .join("wasmer-cache")
}

fn load_or_compile_module(store: &Store, wasm_bytes: &[u8]) -> Result<Module> {
    let key = CacheHash::generate(wasm_bytes);
    let mut cache = FileSystemCache::new(wasmer_cache_dir())
        .context("failed to create/access Wasmer cache directory")?;

    if let Ok(module) = unsafe { cache.load(store, key) } {
        return Ok(module);
    }

    let module = Module::new(store, wasm_bytes).context("failed to compile wasm module")?;
    let _ = cache.store(key, &module);
    Ok(module)
}

// ============================================================
// Guest memory helpers
// ============================================================

fn write_guest_bytes(env: &mut FunctionEnvMut<RuntimeEnv>, guest_ptr: u32, data: &[u8]) -> bool {
    let (state, store) = env.data_and_store_mut();
    let Some(memory) = state.memory.clone() else {
        return false;
    };
    let view = memory.view(&store);
    view.write(guest_ptr as u64, data).is_ok()
}

fn write_guest_u32(env: &mut FunctionEnvMut<RuntimeEnv>, guest_ptr: u32, val: u32) -> bool {
    write_guest_bytes(env, guest_ptr, &val.to_le_bytes())
}

fn write_guest_i32(env: &mut FunctionEnvMut<RuntimeEnv>, guest_ptr: u32, val: i32) -> bool {
    write_guest_bytes(env, guest_ptr, &val.to_le_bytes())
}

fn write_guest_u64(env: &mut FunctionEnvMut<RuntimeEnv>, guest_ptr: u32, val: u64) -> bool {
    write_guest_bytes(env, guest_ptr, &val.to_le_bytes())
}

fn write_guest_i64(env: &mut FunctionEnvMut<RuntimeEnv>, guest_ptr: u32, val: i64) -> bool {
    write_guest_bytes(env, guest_ptr, &val.to_le_bytes())
}

fn write_guest_f64(env: &mut FunctionEnvMut<RuntimeEnv>, guest_ptr: u32, val: f64) -> bool {
    write_guest_bytes(env, guest_ptr, &val.to_le_bytes())
}

fn write_guest_u8(env: &mut FunctionEnvMut<RuntimeEnv>, guest_ptr: u32, val: u8) -> bool {
    write_guest_bytes(env, guest_ptr, &[val])
}

fn read_guest_bytes(
    env: &mut FunctionEnvMut<RuntimeEnv>,
    guest_ptr: i32,
    len: usize,
) -> Option<Vec<u8>> {
    if guest_ptr < 0 {
        return None;
    }
    let (state, store) = env.data_and_store_mut();
    let memory = state.memory.clone()?;
    let view = memory.view(&store);
    let mut out = vec![0u8; len];
    view.read(guest_ptr as u64, &mut out).ok()?;
    Some(out)
}

fn read_guest_u32_array(
    env: &mut FunctionEnvMut<RuntimeEnv>,
    guest_ptr: i32,
    count: usize,
) -> Option<Vec<u32>> {
    let bytes = read_guest_bytes(env, guest_ptr, count * 4)?;
    let mut result = Vec::with_capacity(count);
    for chunk in bytes.chunks_exact(4) {
        result.push(u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]));
    }
    Some(result)
}

fn read_guest_c_string(env: &mut FunctionEnvMut<RuntimeEnv>, guest_ptr: i32) -> Option<Vec<u8>> {
    if guest_ptr < 0 {
        return None;
    }
    let (state, store) = env.data_and_store_mut();
    let memory = state.memory.clone()?;
    let view = memory.view(&store);
    let mut out = Vec::new();
    let mut offset = guest_ptr as u64;
    for _ in 0..MAX_GUEST_CSTRING_SCAN {
        let mut b = [0u8; 1];
        view.read(offset, &mut b).ok()?;
        if b[0] == 0 {
            return Some(out);
        }
        out.push(b[0]);
        offset += 1;
    }
    None
}

// ============================================================
// WASM import handlers for "napi" module
// ============================================================

// --- Init ---

fn guest_napi_wasm_init_env(_env: FunctionEnvMut<RuntimeEnv>) -> i32 {
    let ok = unsafe { snapi_bridge_init() };
    if ok != 0 { 1 } else { 0 }
}

// --- Singleton getters ---

fn guest_napi_get_undefined(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_undefined(&mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_get_null(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_null(&mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_get_boolean(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, value: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_boolean(value, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_get_global(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_global(&mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

// --- Value creation ---

fn guest_napi_create_string_utf8(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, str_ptr: i32, length: i32, rp: i32) -> i32 {
    let wl = length as u32;
    let sb = if wl == 0xFFFFFFFFu32 { read_guest_c_string(&mut env, str_ptr) } else { read_guest_bytes(&mut env, str_ptr, wl as usize) };
    let Some(sb) = sb else { return 1; };
    let cs = CString::new(sb).unwrap_or_default();
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_string_utf8(cs.as_ptr(), wl, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_create_string_latin1(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, str_ptr: i32, length: i32, rp: i32) -> i32 {
    let wl = length as u32;
    let sb = if wl == 0xFFFFFFFFu32 { read_guest_c_string(&mut env, str_ptr) } else { read_guest_bytes(&mut env, str_ptr, wl as usize) };
    let Some(sb) = sb else { return 1; };
    let cs = CString::new(sb).unwrap_or_default();
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_string_latin1(cs.as_ptr(), wl, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_create_int32(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, value: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_int32(value, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_create_uint32(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, value: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_uint32(value as u32, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_create_double(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, value: f64, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_double(value, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_create_int64(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, value: i64, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_int64(value, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_create_object(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_object(&mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_create_array(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_array(&mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_create_array_with_length(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, length: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_array_with_length(length as u32, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_create_symbol(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, desc: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_symbol(desc as u32, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_create_error(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, code: i32, msg: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_error(code as u32, msg as u32, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_create_type_error(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, code: i32, msg: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_type_error(code as u32, msg as u32, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_create_range_error(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, code: i32, msg: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_range_error(code as u32, msg as u32, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_create_bigint_int64(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, value: i64, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_bigint_int64(value, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_create_bigint_uint64(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, value: i64, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_bigint_uint64(value as u64, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_create_date(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, time: f64, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_date(time, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_create_external(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, data: i32, _finalize_cb: i32, _finalize_hint: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_external(data as u64, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

// --- Value reading ---

fn guest_napi_get_value_string_utf8(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, bp: i32, bs: i32, rp: i32) -> i32 {
    let hbs = if bs <= 0 { 0usize } else { bs as usize };
    let mut hb = vec![0i8; hbs];
    let mut rl: usize = 0;
    let s = unsafe { snapi_bridge_get_value_string_utf8(vh as u32, if hbs > 0 { hb.as_mut_ptr() } else { std::ptr::null_mut() }, hbs, &mut rl) };
    if s != 0 { return s; }
    if bp > 0 && hbs > 0 {
        let n = hbs.min(rl + 1);
        let b: Vec<u8> = hb[..n].iter().map(|&x| x as u8).collect();
        write_guest_bytes(&mut env, bp as u32, &b);
    }
    if rp > 0 { write_guest_u32(&mut env, rp as u32, rl as u32); }
    0
}

fn guest_napi_get_value_string_latin1(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, bp: i32, bs: i32, rp: i32) -> i32 {
    let hbs = if bs <= 0 { 0usize } else { bs as usize };
    let mut hb = vec![0i8; hbs];
    let mut rl: usize = 0;
    let s = unsafe { snapi_bridge_get_value_string_latin1(vh as u32, if hbs > 0 { hb.as_mut_ptr() } else { std::ptr::null_mut() }, hbs, &mut rl) };
    if s != 0 { return s; }
    if bp > 0 && hbs > 0 {
        let n = hbs.min(rl + 1);
        let b: Vec<u8> = hb[..n].iter().map(|&x| x as u8).collect();
        write_guest_bytes(&mut env, bp as u32, &b);
    }
    if rp > 0 { write_guest_u32(&mut env, rp as u32, rl as u32); }
    0
}

fn guest_napi_get_value_int32(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, rp: i32) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_get_value_int32(vh as u32, &mut r) };
    if s == 0 { write_guest_i32(&mut env, rp as u32, r); }
    s
}

fn guest_napi_get_value_uint32(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, rp: i32) -> i32 {
    let mut r: u32 = 0;
    let s = unsafe { snapi_bridge_get_value_uint32(vh as u32, &mut r) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, r); }
    s
}

fn guest_napi_get_value_double(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, rp: i32) -> i32 {
    let mut r: f64 = 0.0;
    let s = unsafe { snapi_bridge_get_value_double(vh as u32, &mut r) };
    if s == 0 { write_guest_f64(&mut env, rp as u32, r); }
    s
}

fn guest_napi_get_value_int64(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, rp: i32) -> i32 {
    let mut r: i64 = 0;
    let s = unsafe { snapi_bridge_get_value_int64(vh as u32, &mut r) };
    if s == 0 { write_guest_i64(&mut env, rp as u32, r); }
    s
}

fn guest_napi_get_value_bool(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, rp: i32) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_get_value_bool(vh as u32, &mut r) };
    if s == 0 { write_guest_u8(&mut env, rp as u32, r as u8); }
    s
}

fn guest_napi_get_value_bigint_int64(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, vp: i32, lp: i32) -> i32 {
    let mut val: i64 = 0;
    let mut lossless: i32 = 0;
    let s = unsafe { snapi_bridge_get_value_bigint_int64(vh as u32, &mut val, &mut lossless) };
    if s == 0 {
        write_guest_i64(&mut env, vp as u32, val);
        write_guest_u8(&mut env, lp as u32, lossless as u8);
    }
    s
}

fn guest_napi_get_value_bigint_uint64(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, vp: i32, lp: i32) -> i32 {
    let mut val: u64 = 0;
    let mut lossless: i32 = 0;
    let s = unsafe { snapi_bridge_get_value_bigint_uint64(vh as u32, &mut val, &mut lossless) };
    if s == 0 {
        write_guest_u64(&mut env, vp as u32, val);
        write_guest_u8(&mut env, lp as u32, lossless as u8);
    }
    s
}

fn guest_napi_get_date_value(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, rp: i32) -> i32 {
    let mut r: f64 = 0.0;
    let s = unsafe { snapi_bridge_get_date_value(vh as u32, &mut r) };
    if s == 0 { write_guest_f64(&mut env, rp as u32, r); }
    s
}

fn guest_napi_get_value_external(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, rp: i32) -> i32 {
    let mut data: u64 = 0;
    let s = unsafe { snapi_bridge_get_value_external(vh as u32, &mut data) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, data as u32); }
    s
}

// --- Type checking ---

fn guest_napi_typeof(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, rp: i32) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_typeof(vh as u32, &mut r) };
    if s == 0 { write_guest_i32(&mut env, rp as u32, r); }
    s
}

macro_rules! guest_is_check {
    ($name:ident, $bridge:ident) => {
        fn $name(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, rp: i32) -> i32 {
            let mut r: i32 = 0;
            let s = unsafe { $bridge(vh as u32, &mut r) };
            if s == 0 { write_guest_u8(&mut env, rp as u32, r as u8); }
            s
        }
    };
}

guest_is_check!(guest_napi_is_array, snapi_bridge_is_array);
guest_is_check!(guest_napi_is_error, snapi_bridge_is_error);
guest_is_check!(guest_napi_is_arraybuffer, snapi_bridge_is_arraybuffer);
guest_is_check!(guest_napi_is_typedarray, snapi_bridge_is_typedarray);
guest_is_check!(guest_napi_is_dataview, snapi_bridge_is_dataview);
guest_is_check!(guest_napi_is_date, snapi_bridge_is_date);
guest_is_check!(guest_napi_is_promise, snapi_bridge_is_promise);

fn guest_napi_instanceof(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, obj: i32, ctor: i32, rp: i32) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_instanceof(obj as u32, ctor as u32, &mut r) };
    if s == 0 { write_guest_u8(&mut env, rp as u32, r as u8); }
    s
}

// --- Coercion ---

macro_rules! guest_coerce {
    ($name:ident, $bridge:ident) => {
        fn $name(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, rp: i32) -> i32 {
            let mut out: u32 = 0;
            let s = unsafe { $bridge(vh as u32, &mut out) };
            if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
            s
        }
    };
}

guest_coerce!(guest_napi_coerce_to_bool, snapi_bridge_coerce_to_bool);
guest_coerce!(guest_napi_coerce_to_number, snapi_bridge_coerce_to_number);
guest_coerce!(guest_napi_coerce_to_string, snapi_bridge_coerce_to_string);
guest_coerce!(guest_napi_coerce_to_object, snapi_bridge_coerce_to_object);

// --- Object operations ---

fn guest_napi_set_property(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32, k: i32, v: i32) -> i32 {
    unsafe { snapi_bridge_set_property(o as u32, k as u32, v as u32) }
}

fn guest_napi_get_property(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32, k: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_property(o as u32, k as u32, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_has_property(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32, k: i32, rp: i32) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_has_property(o as u32, k as u32, &mut r) };
    if s == 0 { write_guest_u8(&mut env, rp as u32, r as u8); }
    s
}

fn guest_napi_has_own_property(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32, k: i32, rp: i32) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_has_own_property(o as u32, k as u32, &mut r) };
    if s == 0 { write_guest_u8(&mut env, rp as u32, r as u8); }
    s
}

fn guest_napi_delete_property(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32, k: i32, rp: i32) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_delete_property(o as u32, k as u32, &mut r) };
    if s == 0 { write_guest_u8(&mut env, rp as u32, r as u8); }
    s
}

fn guest_napi_set_named_property(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32, np: i32, v: i32) -> i32 {
    let Some(nb) = read_guest_c_string(&mut env, np) else { return 1; };
    let cn = CString::new(nb).unwrap_or_default();
    unsafe { snapi_bridge_set_named_property(o as u32, cn.as_ptr(), v as u32) }
}

fn guest_napi_get_named_property(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32, np: i32, rp: i32) -> i32 {
    let Some(nb) = read_guest_c_string(&mut env, np) else { return 1; };
    let cn = CString::new(nb).unwrap_or_default();
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_named_property(o as u32, cn.as_ptr(), &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_has_named_property(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32, np: i32, rp: i32) -> i32 {
    let Some(nb) = read_guest_c_string(&mut env, np) else { return 1; };
    let cn = CString::new(nb).unwrap_or_default();
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_has_named_property(o as u32, cn.as_ptr(), &mut r) };
    if s == 0 { write_guest_u8(&mut env, rp as u32, r as u8); }
    s
}

fn guest_napi_set_element(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32, idx: i32, v: i32) -> i32 {
    unsafe { snapi_bridge_set_element(o as u32, idx as u32, v as u32) }
}

fn guest_napi_get_element(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32, idx: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_element(o as u32, idx as u32, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_has_element(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32, idx: i32, rp: i32) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_has_element(o as u32, idx as u32, &mut r) };
    if s == 0 { write_guest_u8(&mut env, rp as u32, r as u8); }
    s
}

fn guest_napi_delete_element(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32, idx: i32, rp: i32) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_delete_element(o as u32, idx as u32, &mut r) };
    if s == 0 { write_guest_u8(&mut env, rp as u32, r as u8); }
    s
}

fn guest_napi_get_array_length(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, ah: i32, rp: i32) -> i32 {
    let mut r: u32 = 0;
    let s = unsafe { snapi_bridge_get_array_length(ah as u32, &mut r) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, r); }
    s
}

fn guest_napi_get_property_names(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_property_names(o as u32, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_get_all_property_names(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32, mode: i32, filter: i32, conversion: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_all_property_names(o as u32, mode, filter, conversion, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_get_prototype(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_prototype(o as u32, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_object_freeze(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32) -> i32 {
    unsafe { snapi_bridge_object_freeze(o as u32) }
}

fn guest_napi_object_seal(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32) -> i32 {
    unsafe { snapi_bridge_object_seal(o as u32) }
}

// --- Comparison ---

fn guest_napi_strict_equals(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, lhs: i32, rhs: i32, rp: i32) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_strict_equals(lhs as u32, rhs as u32, &mut r) };
    if s == 0 { write_guest_u8(&mut env, rp as u32, r as u8); }
    s
}

// --- Error handling ---

fn guest_napi_throw(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, err: i32) -> i32 {
    unsafe { snapi_bridge_throw(err as u32) }
}

fn guest_napi_throw_error(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, code_ptr: i32, msg_ptr: i32) -> i32 {
    let code_bytes = if code_ptr != 0 { read_guest_c_string(&mut env, code_ptr) } else { None };
    let Some(msg_bytes) = read_guest_c_string(&mut env, msg_ptr) else { return 1; };
    let c_code = code_bytes.map(|b| CString::new(b).unwrap_or_default());
    let c_msg = CString::new(msg_bytes).unwrap_or_default();
    unsafe { snapi_bridge_throw_error(c_code.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()), c_msg.as_ptr()) }
}

fn guest_napi_throw_type_error(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, code_ptr: i32, msg_ptr: i32) -> i32 {
    let code_bytes = if code_ptr != 0 { read_guest_c_string(&mut env, code_ptr) } else { None };
    let Some(msg_bytes) = read_guest_c_string(&mut env, msg_ptr) else { return 1; };
    let c_code = code_bytes.map(|b| CString::new(b).unwrap_or_default());
    let c_msg = CString::new(msg_bytes).unwrap_or_default();
    unsafe { snapi_bridge_throw_type_error(c_code.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()), c_msg.as_ptr()) }
}

fn guest_napi_throw_range_error(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, code_ptr: i32, msg_ptr: i32) -> i32 {
    let code_bytes = if code_ptr != 0 { read_guest_c_string(&mut env, code_ptr) } else { None };
    let Some(msg_bytes) = read_guest_c_string(&mut env, msg_ptr) else { return 1; };
    let c_code = code_bytes.map(|b| CString::new(b).unwrap_or_default());
    let c_msg = CString::new(msg_bytes).unwrap_or_default();
    unsafe { snapi_bridge_throw_range_error(c_code.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()), c_msg.as_ptr()) }
}

fn guest_napi_is_exception_pending(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_is_exception_pending(&mut r) };
    if s == 0 { write_guest_u8(&mut env, rp as u32, r as u8); }
    s
}

fn guest_napi_get_and_clear_last_exception(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_and_clear_last_exception(&mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

// --- Promise ---

fn guest_napi_create_promise(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, dp: i32, rp: i32) -> i32 {
    let mut deferred_id: u32 = 0;
    let mut promise_id: u32 = 0;
    let s = unsafe { snapi_bridge_create_promise(&mut deferred_id, &mut promise_id) };
    if s == 0 {
        write_guest_u32(&mut env, dp as u32, deferred_id);
        write_guest_u32(&mut env, rp as u32, promise_id);
    }
    s
}

fn guest_napi_resolve_deferred(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, d: i32, v: i32) -> i32 {
    unsafe { snapi_bridge_resolve_deferred(d as u32, v as u32) }
}

fn guest_napi_reject_deferred(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, d: i32, v: i32) -> i32 {
    unsafe { snapi_bridge_reject_deferred(d as u32, v as u32) }
}

// --- ArrayBuffer ---

fn guest_napi_create_arraybuffer(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, byte_length: i32, data_ptr: i32, rp: i32) -> i32 {
    // Try to create a guest-memory-backed ArrayBuffer (for WASIX)
    let malloc_fn = env.data().malloc_fn.clone();
    let memory = env.data().memory.clone();

    if let (Some(malloc_fn), Some(memory)) = (malloc_fn, memory) {
        // Allocate memory in the guest's linear memory
        let guest_ptr: i32 = {
            let (_, mut store_ref) = env.data_and_store_mut();
            match malloc_fn.call(&mut store_ref, byte_length) {
                Ok(ptr) if ptr > 0 => ptr,
                _ => return 1, // allocation failed
            }
        };

        // Get host pointer corresponding to the guest allocation
        let host_addr: u64 = {
            let (_, store_ref) = env.data_and_store_mut();
            let view = memory.view(&store_ref);
            let host_base = view.data_ptr() as u64;
            host_base + guest_ptr as u64
        };

        // Zero-initialize the guest memory region
        {
            let zeros = vec![0u8; byte_length as usize];
            write_guest_bytes(&mut env, guest_ptr as u32, &zeros);
        }

        // Create external arraybuffer backed by guest memory
        let mut out: u32 = 0;
        let s = unsafe { snapi_bridge_create_external_arraybuffer(host_addr, byte_length as u32, &mut out) };
        if s == 0 {
            write_guest_u32(&mut env, rp as u32, out);
            if data_ptr > 0 {
                write_guest_u32(&mut env, data_ptr as u32, guest_ptr as u32);
            }
        }
        s
    } else {
        // Fallback: host-memory-backed arraybuffer (non-WASIX path)
        let mut out: u32 = 0;
        let s = unsafe { snapi_bridge_create_arraybuffer(byte_length as u32, &mut out) };
        if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
        s
    }
}

fn guest_napi_get_arraybuffer_info(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, data_ptr: i32, len_ptr: i32) -> i32 {
    let mut host_data_addr: u64 = 0;
    let mut bl: u32 = 0;
    let s = unsafe { snapi_bridge_get_arraybuffer_info(vh as u32, &mut host_data_addr, &mut bl) };
    if s != 0 { return s; }

    if len_ptr > 0 { write_guest_u32(&mut env, len_ptr as u32, bl); }

    if data_ptr > 0 && host_data_addr != 0 {
        // Convert host pointer back to guest pointer
        let memory = env.data().memory.clone();
        if let Some(memory) = memory {
            let guest_data_ptr: u32 = {
                let (_, store_ref) = env.data_and_store_mut();
                let view = memory.view(&store_ref);
                let host_base = view.data_ptr() as u64;
                (host_data_addr - host_base) as u32
            };
            write_guest_u32(&mut env, data_ptr as u32, guest_data_ptr);
        }
    }
    0
}

fn guest_napi_detach_arraybuffer(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32) -> i32 {
    unsafe { snapi_bridge_detach_arraybuffer(vh as u32) }
}

fn guest_napi_is_detached_arraybuffer(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, rp: i32) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_is_detached_arraybuffer(vh as u32, &mut r) };
    if s == 0 { write_guest_u8(&mut env, rp as u32, r as u8); }
    s
}

// --- TypedArray ---

fn guest_napi_create_typedarray(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, typ: i32, length: i32, ab: i32, offset: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_typedarray(typ, length as u32, ab as u32, offset as u32, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_get_typedarray_info(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, tp: i32, lp: i32, _dp: i32, abp: i32, bop: i32) -> i32 {
    let mut typ: i32 = 0;
    let mut len: u32 = 0;
    let mut ab: u32 = 0;
    let mut bo: u32 = 0;
    let s = unsafe { snapi_bridge_get_typedarray_info(vh as u32, &mut typ, &mut len, &mut ab, &mut bo) };
    if s == 0 {
        if tp > 0 { write_guest_i32(&mut env, tp as u32, typ); }
        if lp > 0 { write_guest_u32(&mut env, lp as u32, len); }
        if abp > 0 { write_guest_u32(&mut env, abp as u32, ab); }
        if bop > 0 { write_guest_u32(&mut env, bop as u32, bo); }
    }
    s
}

// --- DataView ---

fn guest_napi_create_dataview(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, bl: i32, ab: i32, bo: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_dataview(bl as u32, ab as u32, bo as u32, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

fn guest_napi_get_dataview_info(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, blp: i32, _dp: i32, abp: i32, bop: i32) -> i32 {
    let mut bl: u32 = 0;
    let mut ab: u32 = 0;
    let mut bo: u32 = 0;
    let s = unsafe { snapi_bridge_get_dataview_info(vh as u32, &mut bl, &mut ab, &mut bo) };
    if s == 0 {
        if blp > 0 { write_guest_u32(&mut env, blp as u32, bl); }
        if abp > 0 { write_guest_u32(&mut env, abp as u32, ab); }
        if bop > 0 { write_guest_u32(&mut env, bop as u32, bo); }
    }
    s
}

// --- References ---

fn guest_napi_create_reference(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, irc: i32, rp: i32) -> i32 {
    let mut ref_id: u32 = 0;
    let s = unsafe { snapi_bridge_create_reference(vh as u32, irc as u32, &mut ref_id) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, ref_id); }
    s
}

fn guest_napi_delete_reference(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, r: i32) -> i32 {
    unsafe { snapi_bridge_delete_reference(r as u32) }
}

fn guest_napi_reference_ref(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, r: i32, rp: i32) -> i32 {
    let mut count: u32 = 0;
    let s = unsafe { snapi_bridge_reference_ref(r as u32, &mut count) };
    if s == 0 && rp > 0 { write_guest_u32(&mut env, rp as u32, count); }
    s
}

fn guest_napi_reference_unref(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, r: i32, rp: i32) -> i32 {
    let mut count: u32 = 0;
    let s = unsafe { snapi_bridge_reference_unref(r as u32, &mut count) };
    if s == 0 && rp > 0 { write_guest_u32(&mut env, rp as u32, count); }
    s
}

fn guest_napi_get_reference_value(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, r: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_reference_value(r as u32, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

// --- Handle scopes ---

fn guest_napi_open_handle_scope(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, _rp: i32) -> i32 { 0 }
fn guest_napi_close_handle_scope(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, _scope: i32) -> i32 { 0 }

fn guest_napi_open_escapable_handle_scope(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    let mut scope_id: u32 = 0;
    let s = unsafe { snapi_bridge_open_escapable_handle_scope(&mut scope_id) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, scope_id); }
    s
}

fn guest_napi_close_escapable_handle_scope(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, scope: i32) -> i32 {
    unsafe { snapi_bridge_close_escapable_handle_scope(scope as u32) }
}

fn guest_napi_escape_handle(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, scope: i32, escapee: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_escape_handle(scope as u32, escapee as u32, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

// --- Type tagging ---

fn guest_napi_type_tag_object(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, obj: i32, tag_ptr: i32) -> i32 {
    // napi_type_tag is { uint64_t lower; uint64_t upper; } = 16 bytes
    let Some(tag_bytes) = read_guest_bytes(&mut env, tag_ptr, 16) else { return 1; };
    let lower = u64::from_le_bytes(tag_bytes[0..8].try_into().unwrap());
    let upper = u64::from_le_bytes(tag_bytes[8..16].try_into().unwrap());
    unsafe { snapi_bridge_type_tag_object(obj as u32, lower, upper) }
}

fn guest_napi_check_object_type_tag(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, obj: i32, tag_ptr: i32, rp: i32) -> i32 {
    let Some(tag_bytes) = read_guest_bytes(&mut env, tag_ptr, 16) else { return 1; };
    let lower = u64::from_le_bytes(tag_bytes[0..8].try_into().unwrap());
    let upper = u64::from_le_bytes(tag_bytes[8..16].try_into().unwrap());
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_check_object_type_tag(obj as u32, lower, upper, &mut r) };
    if s == 0 { write_guest_u8(&mut env, rp as u32, r as u8); }
    s
}

// --- Function calling ---

fn guest_napi_call_function(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, recv: i32, func: i32, argc: i32, argv_ptr: i32, rp: i32) -> i32 {
    let argc_u = argc as u32;
    let argv_ids = if argc_u > 0 {
        let Some(ids) = read_guest_u32_array(&mut env, argv_ptr, argc_u as usize) else { return 1; };
        ids
    } else {
        vec![]
    };
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_call_function(recv as u32, func as u32, argc_u, argv_ids.as_ptr(), &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

// --- Script execution ---

fn guest_napi_run_script(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, sh: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_run_script(sh as u32, &mut out) };
    if s == 0 { write_guest_u32(&mut env, rp as u32, out); }
    s
}

// --- Misc stubs ---

fn guest_napi_get_last_error_info(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, _rp: i32) -> i32 { 0 }

fn guest_napi_get_version(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    write_guest_u32(&mut env, rp as u32, 8);
    0
}

// ============================================================
// Register all WASM imports for the "napi" module
// ============================================================

fn register_napi_imports(store: &mut Store, fe: &FunctionEnv<RuntimeEnv>, io: &mut Imports) {
    macro_rules! reg {
        ($name:expr, $func:expr) => {
            io.define("napi", $name, Function::new_typed_with_env(store, fe, $func));
        };
    }

    // Init
    reg!("napi_wasm_init_env", guest_napi_wasm_init_env);
    // Singleton getters
    reg!("napi_get_undefined", guest_napi_get_undefined);
    reg!("napi_get_null", guest_napi_get_null);
    reg!("napi_get_boolean", guest_napi_get_boolean);
    reg!("napi_get_global", guest_napi_get_global);
    // Value creation
    reg!("napi_create_string_utf8", guest_napi_create_string_utf8);
    reg!("napi_create_string_latin1", guest_napi_create_string_latin1);
    reg!("napi_create_int32", guest_napi_create_int32);
    reg!("napi_create_uint32", guest_napi_create_uint32);
    reg!("napi_create_double", guest_napi_create_double);
    reg!("napi_create_int64", guest_napi_create_int64);
    reg!("napi_create_object", guest_napi_create_object);
    reg!("napi_create_array", guest_napi_create_array);
    reg!("napi_create_array_with_length", guest_napi_create_array_with_length);
    reg!("napi_create_symbol", guest_napi_create_symbol);
    reg!("napi_create_error", guest_napi_create_error);
    reg!("napi_create_type_error", guest_napi_create_type_error);
    reg!("napi_create_range_error", guest_napi_create_range_error);
    reg!("napi_create_bigint_int64", guest_napi_create_bigint_int64);
    reg!("napi_create_bigint_uint64", guest_napi_create_bigint_uint64);
    reg!("napi_create_date", guest_napi_create_date);
    reg!("napi_create_external", guest_napi_create_external);
    reg!("napi_create_arraybuffer", guest_napi_create_arraybuffer);
    reg!("napi_create_typedarray", guest_napi_create_typedarray);
    reg!("napi_create_dataview", guest_napi_create_dataview);
    reg!("napi_create_promise", guest_napi_create_promise);
    // Value reading
    reg!("napi_get_value_string_utf8", guest_napi_get_value_string_utf8);
    reg!("napi_get_value_string_latin1", guest_napi_get_value_string_latin1);
    reg!("napi_get_value_int32", guest_napi_get_value_int32);
    reg!("napi_get_value_uint32", guest_napi_get_value_uint32);
    reg!("napi_get_value_double", guest_napi_get_value_double);
    reg!("napi_get_value_int64", guest_napi_get_value_int64);
    reg!("napi_get_value_bool", guest_napi_get_value_bool);
    reg!("napi_get_value_bigint_int64", guest_napi_get_value_bigint_int64);
    reg!("napi_get_value_bigint_uint64", guest_napi_get_value_bigint_uint64);
    reg!("napi_get_date_value", guest_napi_get_date_value);
    reg!("napi_get_value_external", guest_napi_get_value_external);
    // Type checking
    reg!("napi_typeof", guest_napi_typeof);
    reg!("napi_is_array", guest_napi_is_array);
    reg!("napi_is_error", guest_napi_is_error);
    reg!("napi_is_arraybuffer", guest_napi_is_arraybuffer);
    reg!("napi_is_typedarray", guest_napi_is_typedarray);
    reg!("napi_is_dataview", guest_napi_is_dataview);
    reg!("napi_is_date", guest_napi_is_date);
    reg!("napi_is_promise", guest_napi_is_promise);
    reg!("napi_instanceof", guest_napi_instanceof);
    // Coercion
    reg!("napi_coerce_to_bool", guest_napi_coerce_to_bool);
    reg!("napi_coerce_to_number", guest_napi_coerce_to_number);
    reg!("napi_coerce_to_string", guest_napi_coerce_to_string);
    reg!("napi_coerce_to_object", guest_napi_coerce_to_object);
    // Object operations
    reg!("napi_set_property", guest_napi_set_property);
    reg!("napi_get_property", guest_napi_get_property);
    reg!("napi_has_property", guest_napi_has_property);
    reg!("napi_has_own_property", guest_napi_has_own_property);
    reg!("napi_delete_property", guest_napi_delete_property);
    reg!("napi_set_named_property", guest_napi_set_named_property);
    reg!("napi_get_named_property", guest_napi_get_named_property);
    reg!("napi_has_named_property", guest_napi_has_named_property);
    reg!("napi_set_element", guest_napi_set_element);
    reg!("napi_get_element", guest_napi_get_element);
    reg!("napi_has_element", guest_napi_has_element);
    reg!("napi_delete_element", guest_napi_delete_element);
    reg!("napi_get_array_length", guest_napi_get_array_length);
    reg!("napi_get_property_names", guest_napi_get_property_names);
    reg!("napi_get_all_property_names", guest_napi_get_all_property_names);
    reg!("napi_get_prototype", guest_napi_get_prototype);
    reg!("napi_object_freeze", guest_napi_object_freeze);
    reg!("napi_object_seal", guest_napi_object_seal);
    // Comparison
    reg!("napi_strict_equals", guest_napi_strict_equals);
    // Error handling
    reg!("napi_throw", guest_napi_throw);
    reg!("napi_throw_error", guest_napi_throw_error);
    reg!("napi_throw_type_error", guest_napi_throw_type_error);
    reg!("napi_throw_range_error", guest_napi_throw_range_error);
    reg!("napi_is_exception_pending", guest_napi_is_exception_pending);
    reg!("napi_get_and_clear_last_exception", guest_napi_get_and_clear_last_exception);
    // Promise
    reg!("napi_resolve_deferred", guest_napi_resolve_deferred);
    reg!("napi_reject_deferred", guest_napi_reject_deferred);
    // ArrayBuffer
    reg!("napi_get_arraybuffer_info", guest_napi_get_arraybuffer_info);
    reg!("napi_detach_arraybuffer", guest_napi_detach_arraybuffer);
    reg!("napi_is_detached_arraybuffer", guest_napi_is_detached_arraybuffer);
    // TypedArray
    reg!("napi_get_typedarray_info", guest_napi_get_typedarray_info);
    // DataView
    reg!("napi_get_dataview_info", guest_napi_get_dataview_info);
    // References
    reg!("napi_create_reference", guest_napi_create_reference);
    reg!("napi_delete_reference", guest_napi_delete_reference);
    reg!("napi_reference_ref", guest_napi_reference_ref);
    reg!("napi_reference_unref", guest_napi_reference_unref);
    reg!("napi_get_reference_value", guest_napi_get_reference_value);
    // Handle scopes
    reg!("napi_open_handle_scope", guest_napi_open_handle_scope);
    reg!("napi_close_handle_scope", guest_napi_close_handle_scope);
    reg!("napi_open_escapable_handle_scope", guest_napi_open_escapable_handle_scope);
    reg!("napi_close_escapable_handle_scope", guest_napi_close_escapable_handle_scope);
    reg!("napi_escape_handle", guest_napi_escape_handle);
    // Type tagging
    reg!("napi_type_tag_object", guest_napi_type_tag_object);
    reg!("napi_check_object_type_tag", guest_napi_check_object_type_tag);
    // Function calling
    reg!("napi_call_function", guest_napi_call_function);
    // Script execution
    reg!("napi_run_script", guest_napi_run_script);
    // Misc
    reg!("napi_get_last_error_info", guest_napi_get_last_error_info);
    reg!("napi_get_version", guest_napi_get_version);
}

// ============================================================
// Public API
// ============================================================

pub fn run_wasm_main_i32(wasm_path: &Path) -> Result<i32> {
    let wasm_bytes = std::fs::read(wasm_path)
        .with_context(|| format!("failed to read wasm file at {}", wasm_path.display()))?;
    let mut store = make_store();
    let module = load_or_compile_module(&store, &wasm_bytes)?;

    let memory_type = module
        .imports()
        .find_map(|import| {
            if import.module() == "env" && import.name() == "memory" {
                if let wasmer::ExternType::Memory(ty) = import.ty() {
                    return Some(*ty);
                }
            }
            None
        })
        .context("module does not import env.memory")?;
    let memory = Memory::new(&mut store, memory_type).context("failed to create memory")?;

    let func_env = FunctionEnv::new(&mut store, RuntimeEnv {
        memory: Some(memory.clone()),
        malloc_fn: None,
    });

    let mut import_object = imports! {
        "env" => {
            "memory" => memory,
        },
    };
    register_napi_imports(&mut store, &func_env, &mut import_object);

    let instance = Instance::new(&mut store, &module, &import_object)
        .context("failed to instantiate wasm module")?;
    let main_fn: TypedFunction<(), i32> = instance
        .exports
        .get_typed_function(&store, "main")
        .context("no main export found")?;
    let result = main_fn.call(&mut store).unwrap_or(-1);
    Ok(result)
}

pub fn run_wasix_main_capture_stdout(wasm_path: &Path, args: &[&str]) -> Result<(i32, String)> {
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .context("failed to create tokio runtime for WASIX")?;
    let _guard = runtime.enter();

    let (stdout_tx, mut stdout_rx) = Pipe::channel();
    let exit_code = {
        let wasm_bytes = std::fs::read(wasm_path)
            .with_context(|| format!("failed to read wasm file at {}", wasm_path.display()))?;
        let mut store = make_store();
        let module = load_or_compile_module(&store, &wasm_bytes)?;
        let mut builder = WasiEnv::builder("guest-test")
            .engine(store.engine().clone())
            .stdout(Box::new(stdout_tx));
        if !args.is_empty() {
            builder = builder.args(args.iter().copied());
        }

        let mut wasi_env = builder
            .finalize(&mut store)
            .context("failed to finalize WASIX environment")?;
        let mut import_object = wasi_env
            .import_object_for_all_wasi_versions(&mut store, &module)
            .context("failed to generate WASIX imports")?;

        let imported_memory_type = module
            .imports()
            .find_map(|import| {
                if import.module() == "env" && import.name() == "memory" {
                    if let wasmer::ExternType::Memory(ty) = import.ty() {
                        return Some(*ty);
                    }
                }
                None
            })
            .context("WASIX module does not import env.memory")?;
        let memory = Memory::new(&mut store, imported_memory_type)
            .context("failed to create imported WASIX memory")?;
        import_object.define("env", "memory", memory.clone());

        let func_env = FunctionEnv::new(&mut store, RuntimeEnv {
            memory: Some(memory),
            malloc_fn: None,
        });
        register_napi_imports(&mut store, &func_env, &mut import_object);

        let instance = Instance::new(&mut store, &module, &import_object)
            .context("failed to instantiate WASIX wasm module")?;

        // Store guest's malloc function for guest-memory-backed ArrayBuffers
        if let Ok(malloc) = instance.exports.get_typed_function::<i32, i32>(&store, "malloc") {
            func_env.as_mut(&mut store).malloc_fn = Some(malloc);
        }

        wasi_env
            .initialize(&mut store, instance.clone())
            .context("failed to initialize WASIX environment")?;

        let exit = if let Ok(main) = instance
            .exports
            .get_typed_function::<(i32, i32), i32>(&mut store, "main")
        {
            if let Ok(ctors) = instance
                .exports
                .get_typed_function::<(), ()>(&mut store, "__wasm_call_ctors")
            {
                let _ = ctors.call(&mut store);
            }
            main.call(&mut store, 0, 0)
                .map_err(|err| anyhow::anyhow!("WASIX `main` call failed: {err:?}"))?
        } else {
            let start: TypedFunction<(), ()> = instance
                .exports
                .get_typed_function(&mut store, "_start")
                .context("failed to find export `_start`")?;
            let exit = match start.call(&mut store) {
                Ok(()) => 0,
                Err(err) => {
                    if let Some(WasiError::Exit(code)) = err.downcast_ref::<WasiError>() {
                        i32::from(*code)
                    } else {
                        return Err(anyhow::anyhow!("WASIX `_start` failed: {err}"));
                    }
                }
            };
            drop(start);
            exit
        };

        drop(instance);
        drop(import_object);
        drop(wasi_env);
        drop(store);
        exit
    };

    let mut out = String::new();
    block_on(stdout_rx.read_to_string(&mut out))
        .context("failed reading WASIX stdout pipe")?;
    Ok((exit_code, out))
}
