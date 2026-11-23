fn main() {
    // The MFT scanner opens \\.\X: with GENERIC_READ, which needs an elevated
    // token, so the exe requests administrator rights like the Qt build did.
    println!("cargo:rerun-if-changed=app.manifest");
    let windows = tauri_build::WindowsAttributes::new().app_manifest(include_str!("app.manifest"));
    let attrs = tauri_build::Attributes::new().windows_attributes(windows);
    tauri_build::try_build(attrs).expect("failed to run tauri-build");
}
