use anyhow::{bail, Result};
use napi_wasmer::{run_wasix_main_capture_stdout, run_wasm_main_i32};
use std::path::Path;

fn main() -> Result<()> {
    let mut args = std::env::args().skip(1);
    let wasm_path = match args.next() {
        Some(p) => p,
        None => {
            bail!("usage: cargo run -p napi_wasmer -- <wasm-file> [wasix|main]");
        }
    };
    let entry = args.next().unwrap_or_else(|| "wasix".to_string());
    let wasm_path = Path::new(&wasm_path);

    if entry == "wasix" {
        let (exit, stdout) = run_wasix_main_capture_stdout(wasm_path, &[])?;
        print!("{stdout}");
        println!("wasix_exit_code={exit}");
        return Ok(());
    }

    let result = run_wasm_main_i32(wasm_path)?;
    println!("main() => {result}");
    Ok(())
}
