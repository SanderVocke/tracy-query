use std::fs;
use std::process::Command;

#[test]
fn unsupported_attributes_and_async_are_rejected() {
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
    ] {
        let directory = root.join("target/compile-fail").join(name);
        fs::create_dir_all(directory.join("src")).unwrap();
        fs::write(directory.join("src/main.rs"), source).unwrap();
        fs::write(
            directory.join("Cargo.toml"),
            format!(
                "[package]\nname='compile-fail-{name}'\nversion='0.0.0'\nedition='2021'\n[dependencies]\ntracy-nextest-capture-macros={{path='{}'}}\n",
                root.join("../tracy-nextest-capture-macros").display()
            ),
        )
        .unwrap();
        let output = Command::new(env!("CARGO"))
            .args(["check", "--quiet", "--manifest-path"])
            .arg(directory.join("Cargo.toml"))
            .output()
            .unwrap();
        assert!(!output.status.success(), "{name} unexpectedly compiled");
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(stderr.contains(expected), "{name}: {stderr}");
    }
}
