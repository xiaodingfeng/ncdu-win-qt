mod cleanup;
mod ai;
mod appmove;
mod appsync;
mod config;
mod elevate;
mod index;
mod legacy;
mod locker;
mod mft;
mod misc;
mod ops;
mod scan;
mod settings;
mod sysopt;

use tauri::Manager;

#[tauri::command]
fn load_settings(app: tauri::AppHandle) -> Result<settings::AppSettings, String> {
    settings::load_or_init(&app)
}

#[tauri::command]
fn save_settings(app: tauri::AppHandle, settings: settings::AppSettings) -> Result<(), String> {
    settings::save(&app, &settings)
}

#[tauri::command]
fn import_legacy_settings(app: tauri::AppHandle) -> Result<legacy::ImportReport, String> {
    settings::import_legacy(&app)
}

#[tauri::command]
fn set_ai_api_key(app: tauri::AppHandle, api_key: String) -> Result<(), String> {
    settings::set_api_key(&app, &api_key)
}

#[tauri::command]
fn clear_ai_api_key(app: tauri::AppHandle) -> Result<(), String> {
    settings::clear_api_key(&app)
}

pub fn is_helper_invocation() -> bool {
    elevate::is_helper_invocation()
}

/// Logs every panic before the process unwinds/aborts so crashes leave a trail.
fn install_panic_hook() {
    let default_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        let location = info
            .location()
            .map(|location| {
                format!(
                    "{}:{}:{}",
                    location.file(),
                    location.line(),
                    location.column()
                )
            })
            .unwrap_or_else(|| "unknown".to_string());
        let payload = info
            .payload()
            .downcast_ref::<&str>()
            .map(|value| (*value).to_string())
            .or_else(|| info.payload().downcast_ref::<String>().cloned())
            .unwrap_or_else(|| "unknown panic".to_string());
        log::error!("[panic] {payload} at {location}");
        default_hook(info);
    }));
}

pub fn run_helper() -> i32 {
    elevate::run_helper_from_args()
}

/// Handles `--apply-update ...`: the staged silent updater, which runs before
/// Tauri starts.
pub fn apply_update_if_requested() -> Option<i32> {
    misc::apply_update_from_args()
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    install_panic_hook();
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(
            tauri_plugin_log::Builder::new()
                .level(log::LevelFilter::Info)
                // Keep the log folder bounded: rotate at 2 MB, keep five files.
                .max_file_size(2 * 1024 * 1024)
                .rotation_strategy(tauri_plugin_log::RotationStrategy::KeepSome(5))
                .timezone_strategy(tauri_plugin_log::TimezoneStrategy::UseLocal)
                .targets([
                    tauri_plugin_log::Target::new(tauri_plugin_log::TargetKind::LogDir {
                        file_name: Some("ncduwin".to_string()),
                    }),
                    tauri_plugin_log::Target::new(tauri_plugin_log::TargetKind::Stdout),
                ])
                .build(),
        )
        .manage(scan::ScanStore::default())
        .manage(cleanup::CleanupStore::default())
        .manage(appsync::AppSyncState::default())
        .manage(index::store::IndexStore::default())
        .setup(|app| {
            log::info!(
                "NcduWin {} starting",
                app.package_info().version
            );
            if let Err(err) = settings::load_or_init(app.handle()) {
                log::error!("initial settings load failed: {err}");
            }
            // Load a saved index snapshot in the background; this never blocks
            // the window and failures only leave the index idle.
            index::commands::prepare(app.handle());
            index::startup(app.handle().clone());
            // The runtime configuration file from older builds is obsolete:
            // configuration is now compiled into the executable.
            if let Ok(dir) = app.path().app_config_dir() {
                let legacy = dir.join("app-config.json");
                if legacy.exists() {
                    let _ = std::fs::remove_file(&legacy);
                    log::info!("removed legacy app-config.json");
                }
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            load_settings,
            save_settings,
            import_legacy_settings,
            set_ai_api_key,
            clear_ai_api_key,
            scan::start_scan,
            scan::cancel_scan,
            scan::get_directory,
            scan::get_treemap,
            scan::search_tree,
            scan::get_scan_root,
            scan::default_path,
            scan::disk_info,
            scan::release_scan,
            ops::delete_paths,
            ops::open_path,
            ops::reveal_path,
            ops::file_times,
            ops::list_dir,
            config::app_config,
            locker::locked_processes,
            locker::close_locking_processes,
            cleanup::scan_cleanup,
            cleanup::scan_duplicates,
            cleanup::cancel_cleanup,
            cleanup::clean_items,
            cleanup::cleanup_disk_space,
            sysopt::get_update_status,
            sysopt::set_update_enabled,
            sysopt::run_network_fix,
            sysopt::flush_dns,
            sysopt::reset_store,
            appmove::appmove_default_roots,
            appmove::scan_app_dirs,
            appmove::move_app_dirs,
            appmove::move_path,
            appmove::restore_app_dir,
            appmove::clean_app_residue,
            appsync::save_folders,
            appsync::set_save_folder,
            appsync::move_save_contents,
            appsync::cancel_save_move,
            ai::ai_get_config,
            ai::ai_save_config,
            ai::ai_fetch_defaults,
            ai::ai_fetch_models,
            ai::ai_chat,
            ai::ai_cancel,
            misc::check_update,
            misc::is_portable,
            misc::submit_feedback,
            misc::log_client,
            misc::download_update,
            index::commands::index_status,
            index::commands::index_volumes,
            index::commands::index_ensure_loaded,
            index::commands::index_build,
            index::commands::index_cancel,
            index::commands::index_search,
            index::commands::index_diagnose,
            index::commands::index_clear
        ])
        .run(tauri::generate_context!())
        .expect("error while running NcduWin");
}
