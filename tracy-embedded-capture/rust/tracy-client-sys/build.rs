use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn feature(name: &str) -> bool {
    env::var_os(format!(
        "CARGO_FEATURE_{}",
        name.replace('-', "_").to_uppercase()
    ))
    .is_some()
}

fn run(command: &mut Command, description: &str) {
    let status = command.status().unwrap_or_else(|error| {
        panic!("cannot run {description}: {error}");
    });
    if !status.success() {
        panic!("{description} failed with {status}");
    }
}

fn run_with_retries(command: &mut Command, description: &str) {
    for attempt in 1..=3 {
        let status = command.status().unwrap_or_else(|error| {
            panic!("cannot run {description}: {error}");
        });
        if status.success() {
            return;
        }
        if attempt != 3 {
            eprintln!("{description} failed on attempt {attempt}; retrying");
            std::thread::sleep(std::time::Duration::from_secs(3));
        } else {
            panic!("{description} failed after 3 attempts with {status}");
        }
    }
}

fn link_search(path: &Path) {
    println!("cargo:rustc-link-search=native={}", path.display());
    for configuration in ["Release", "RelWithDebInfo", "MinSizeRel", "Debug"] {
        println!(
            "cargo:rustc-link-search=native={}",
            path.join(configuration).display()
        );
    }
}

fn main() {
    println!("cargo:rerun-if-env-changed=CMAKE");
    println!("cargo:rerun-if-env-changed=CMAKE_GENERATOR");
    println!("cargo:rerun-if-env-changed=TRACY_QUERY_CMAKE_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=TRACY_QUERY_NATIVE_SANITIZER_LIBS");
    println!("cargo:rerun-if-changed=../../../CMakeLists.txt");
    println!("cargo:rerun-if-changed=../../../cmake/TracyServer.cmake");
    println!("cargo:rerun-if-changed=../../src");
    println!("cargo:rerun-if-changed=../../include/tracy_embedded_capture/embedded_capture.h");

    if env::var_os("DOCS_RS").as_deref() == Some(std::ffi::OsStr::new("1")) || !feature("enable") {
        return;
    }

    let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest
        .join("../../..")
        .canonicalize()
        .expect("repository root");
    let prebuilt = env::var_os("TRACY_QUERY_CMAKE_BUILD_DIR").map(PathBuf::from);
    let build = prebuilt
        .clone()
        .unwrap_or_else(|| PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("native"));
    let cmake = env::var_os("CMAKE").unwrap_or_else(|| "cmake".into());

    let mappings = [
        ("ondemand", "ON_DEMAND"),
        ("manual-lifetime", "MANUAL_LIFETIME"),
        ("delayed-init", "DELAYED_INIT"),
        ("flush-on-exit", "FLUSH_ON_EXIT"),
        ("debuginfod", "DEBUGINFOD"),
        ("only-localhost", "ONLY_LOCALHOST"),
        ("only-ipv4", "ONLY_IPV4"),
        ("timer-fallback", "TIMER_FALLBACK"),
        ("system-tracing", "SYSTEM_TRACING"),
        ("context-switch-tracing", "CONTEXT_SWITCH"),
        ("sampling", "SAMPLING"),
        ("code-transfer", "CODE_TRANSFER"),
        ("broadcast", "BROADCAST"),
        ("callstack-inlines", "CALLSTACK_INLINES"),
        ("crash-handler", "CRASH_HANDLER"),
        ("verify", "VERIFY"),
        ("demangle", "DEMANGLE"),
        ("fibers", "FIBERS"),
    ];
    let target = if feature("embedded-capture") {
        "tracy_embedded_capture_native"
    } else {
        "tracy_instrumentation_client_native"
    };

    if prebuilt.is_none() {
        let mut configure = Command::new(&cmake);
        configure
            .arg("-S")
            .arg(&root)
            .arg("-B")
            .arg(&build)
            .arg("-DCMAKE_BUILD_TYPE=Release")
            .arg("-DBUILD_TESTING=OFF")
            .arg("-DTRACY_QUERY_FULLY_STATIC=OFF")
            .arg(format!(
                "-DTRACY_QUERY_STATIC_MSVC_RUNTIME={}",
                if env::var("CARGO_CFG_TARGET_FEATURE")
                    .unwrap_or_default()
                    .split(',')
                    .any(|feature| feature == "crt-static")
                {
                    "ON"
                } else {
                    "OFF"
                }
            ));
        for (cargo, cmake_name) in mappings {
            configure.arg(format!(
                "-DTRACY_NATIVE_{cmake_name}={}",
                if feature(cargo) { "ON" } else { "OFF" }
            ));
        }
        run_with_retries(
            &mut configure,
            "CMake configure for patched tracy-client-sys",
        );

        let mut compile = Command::new(&cmake);
        compile
            .arg("--build")
            .arg(&build)
            .arg("--config")
            .arg("Release")
            .arg("--target")
            .arg(target)
            .arg("--parallel")
            .arg("2");
        run(&mut compile, "CMake native Tracy build");
    }

    link_search(&build);
    link_search(&build.join("tracy-embedded-capture"));
    println!("cargo:rustc-link-lib=static={target}");
    if feature("embedded-capture") {
        let capstone = build.join("_deps/tracy_capstone-build");
        let zstd = build.join("_deps/tracy_zstd-build/lib");
        link_search(&capstone.join("capstone.dir"));
        link_search(&capstone);
        link_search(&zstd);
        println!("cargo:rustc-link-lib=static=capstone");
        if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
            println!("cargo:rustc-link-lib=static=zstd_static");
        } else {
            println!("cargo:rustc-link-lib=static=zstd");
        }
    }

    if let Ok(libraries) = env::var("TRACY_QUERY_NATIVE_SANITIZER_LIBS") {
        for library in libraries.split(',').filter(|library| !library.is_empty()) {
            println!("cargo:rustc-link-lib=dylib={library}");
        }
    }

    match env::var("CARGO_CFG_TARGET_OS").as_deref() {
        Ok("linux" | "android") => {
            println!("cargo:rustc-link-lib=dl");
            println!("cargo:rustc-link-lib=stdc++");
        }
        Ok("macos" | "ios") => println!("cargo:rustc-link-lib=c++"),
        Ok("freebsd" | "dragonfly") => {
            println!("cargo:rustc-link-lib=c");
            println!("cargo:rustc-link-lib=stdc++");
        }
        Ok("windows") => {
            println!("cargo:rustc-link-lib=user32");
            println!("cargo:rustc-link-lib=advapi32");
            println!("cargo:rustc-link-lib=dbghelp");
            println!("cargo:rustc-link-lib=ws2_32");
        }
        Ok(_) => {}
        Err(error) => panic!("missing target OS: {error}"),
    }
}
