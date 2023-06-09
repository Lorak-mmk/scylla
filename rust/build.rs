use std::{env, path::Path};

fn main() {
    // In this script I assume we don't have invalid unicode paths in Scylla repo.
    let header_dir_env = env::var("DEP_CXX_ASYNC_CXXBRIDGE_DIR1").unwrap();
    assert!(header_dir_env.ends_with("out/cxxbridge/crate"));

    let from_dir = Path::new(&header_dir_env).join("cxx-async/include/rust");

    // OUT_DIR looks like /path/to/scylla/repo/build/dev/rust-dev/build/rust_combined-393e8213db6398b1/out
    let out_dir_env = env::var("OUT_DIR").unwrap();
    let out_dir = Path::new(&out_dir_env);

    // we transform it to /path/to/scylla/repo/build/dev/gen/rust
    let to_dir = out_dir.ancestors().nth(4).unwrap().join("gen/rust");

    fn copy_file(filename: &str, from_dir: &Path, to_dir: &Path) {
        let from = from_dir.join(filename);
        let to = to_dir.join(filename);
        std::fs::copy(&from, &to).unwrap();
        println!("cargo:rerun-if-changed={}", from.to_str().unwrap());
        println!("cargo:rerun-if-changed={}", to.to_str().unwrap());
    }

    copy_file("cxx_async.h", &from_dir, &to_dir);
    copy_file("cxx_async_seastar.h", &from_dir, &to_dir);

    println!("cargo:rerun-if-env-changed=DEP_CXX_ASYNC_CXXBRIDGE_DIR1");
}
