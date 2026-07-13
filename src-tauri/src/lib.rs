mod error;
mod theme;
mod commands;
mod payload;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .invoke_handler(tauri::generate_handler![
            commands::load_launcher_theme,
            commands::load_launcher_settings,
            commands::save_launcher_profile,
            commands::download_and_launch_version,
            commands::minimize_main_window,
            commands::close_main_window,
            commands::start_payload,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
