fn main() {
    let project_root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap();

    // napi-v8 paths
    let napi_v8_dir = project_root.join("napi-v8");
    let napi_v8_include = napi_v8_dir.join("include");
    let napi_v8_src = napi_v8_dir.join("src");

    // V8 paths
    let default_v8_include = "/opt/homebrew/Cellar/v8/14.5.201.9/include".to_string();
    let default_v8_lib = "/opt/homebrew/Cellar/v8/14.5.201.9/lib".to_string();
    let v8_include = std::env::var("V8_INCLUDE_DIR").unwrap_or(default_v8_include);
    let v8_lib = std::env::var("V8_LIB_DIR").unwrap_or(default_v8_lib);

    // Compile the napi-v8 sources + bridge into a single static library
    cc::Build::new()
        .cpp(true)
        .flag_if_supported("-std=c++20")
        .flag_if_supported("-w")
        .define("NAPI_EXTERN", Some(""))
        .define("V8_COMPRESS_POINTERS", Some("1"))
        .define("V8_31BIT_SMIS_ON_64BIT_ARCH", Some("1"))
        .define("V8_ENABLE_SANDBOX", Some("1"))
        .include(&v8_include)
        .include(napi_v8_include.to_str().unwrap())
        .include(napi_v8_src.to_str().unwrap())
        .file("src/napi_bridge_init.cc")
        .file(napi_v8_src.join("js_native_api_v8.cc").to_str().unwrap())
        .file(napi_v8_src.join("unofficial_napi.cc").to_str().unwrap())
        .compile("napi_bridge");

    println!("cargo:rerun-if-changed=src/napi_bridge_init.cc");
    println!("cargo:rustc-link-search=native={v8_lib}");
    println!("cargo:rustc-link-lib=dylib=v8");
    println!("cargo:rustc-link-lib=dylib=v8_libplatform");
    println!("cargo:rustc-link-lib=dylib=v8_libbase");
    println!("cargo:rustc-link-lib=dylib=c++");
}
