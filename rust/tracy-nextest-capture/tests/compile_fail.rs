use std::fs;
use std::process::Command;

fn write_manifest(directory: &std::path::Path, name: &str, dependency: &str) {
    fs::create_dir_all(directory.join("src")).unwrap();
    fs::write(
        directory.join("Cargo.toml"),
        format!(
            "[package]\nname='compile-fail-{name}'\nversion='0.0.0'\nedition='2021'\n[dependencies]\n{dependency}\n",
        ),
    )
    .unwrap();
}

#[test]
fn supported_signatures_compile_and_unsupported_forms_are_rejected() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    for (name, source, expected) in [
        (
            "async",
            "use tracy_nextest_capture_macros::tracy_capture_test; #[tracy_capture_test] async fn bad() {} fn main() {}",
            "async tests are not supported",
        ),
        (
            "should-panic",
            "use tracy_nextest_capture_macros::tracy_capture_test; #[tracy_capture_test] #[should_panic] fn bad() {} fn main() {}",
            "#[should_panic] is not supported",
        ),
        (
            "bad-return",
            "use tracy_nextest_capture_macros::tracy_capture_test; #[tracy_capture_test] fn bad() -> u32 { 1 } fn main() {}",
            "captured tests must return () or Result<(), E>",
        ),
        (
            "incompatible-harness",
            "use tracy_nextest_capture_macros::tracy_capture_test; #[tracy_capture_test] #[rstest] fn bad() {} fn main() {}",
            "incompatible test harness attribute",
        ),
    ] {
        let directory = root.join("target/compile-fail").join(name);
        fs::create_dir_all(directory.join("src")).unwrap();
        fs::write(directory.join("src/main.rs"), source).unwrap();
        write_manifest(
            &directory,
            name,
            &format!(
                "tracy-nextest-capture-macros={{path='{}'}}",
                root.join("../tracy-nextest-capture-macros").display()
            ),
        );
        let output = Command::new(env!("CARGO"))
            .args(["check", "--quiet", "--manifest-path"])
            .arg(directory.join("Cargo.toml"))
            .output()
            .unwrap();
        assert!(!output.status.success(), "{name} unexpectedly compiled");
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(stderr.contains(expected), "{name}: {stderr}");
    }

    let directory = root.join("target/compile-pass/supported");
    fs::create_dir_all(directory.join("src")).unwrap();
    fs::write(
        directory.join("src/main.rs"),
        "use tracy_nextest_capture::tracy_capture_test; #[tracy_capture_test] fn unit() {} #[tracy_capture_test] fn result() -> Result<(), &'static str> { Ok(()) } fn main() {}",
    )
    .unwrap();
    write_manifest(
        &directory,
        "supported",
        &format!(
            "tracy-nextest-capture={{path='{}'}}\n[patch.crates-io]\ntracy-client-sys={{path='{}'}}",
            root.display(), root.join("../tracy-client-sys").display()
        ),
    );
    let status = Command::new(env!("CARGO"))
        .args(["check", "--quiet", "--manifest-path"])
        .arg(directory.join("Cargo.toml"))
        .status()
        .unwrap();
    assert!(status.success(), "supported signatures did not compile");
}

#[test]
fn panic_abort_is_rejected() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let directory = root.join("target/compile-fail/panic-abort");
    fs::create_dir_all(directory.join("src")).unwrap();
    fs::write(directory.join("src/main.rs"), "fn main() {}").unwrap();
    write_manifest(
        &directory,
        "panic-abort",
        &format!(
            "tracy-nextest-capture={{path='{}'}}\n[patch.crates-io]\ntracy-client-sys={{path='{}'}}\n[profile.dev]\npanic='abort'",
            root.display(), root.join("../tracy-client-sys").display()
        ),
    );
    let output = Command::new(env!("CARGO"))
        .args(["check", "--quiet", "--manifest-path"])
        .arg(directory.join("Cargo.toml"))
        .output()
        .unwrap();
    assert!(!output.status.success(), "panic=abort unexpectedly compiled");
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.contains("requires panic=unwind"), "panic-abort: {stderr}");
}
