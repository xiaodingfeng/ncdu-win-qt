// Prevents an extra console window on Windows in release builds.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    // The elevated helper is the same executable; keep Tauri out of its way.
    if ncduwin_lib::is_helper_invocation() {
        std::process::exit(ncduwin_lib::run_helper());
    }
    ncduwin_lib::run()
}
