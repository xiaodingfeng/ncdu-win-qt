// Prevents an extra console window on Windows in release builds.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    // The elevated helper is the same executable; keep Tauri out of its way.
    if ncduwin_lib::is_helper_invocation() {
        std::process::exit(ncduwin_lib::run_helper());
    }
    // The staged updater is a temporary copy of this executable.
    if let Some(code) = ncduwin_lib::apply_update_if_requested() {
        std::process::exit(code);
    }
    ncduwin_lib::run()
}
