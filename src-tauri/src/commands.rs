use tauri::AppHandle;
use tauri::Manager;
use crate::error::LauncherError;
use crate::theme;

#[tauri::command]
pub async fn load_launcher_theme() -> Result<theme::LauncherTheme, LauncherError> {
    theme::load_launcher_theme().await
}

#[tauri::command]
pub fn minimize_main_window(app: AppHandle) -> Result<(), LauncherError> {
    let window = app
        .get_webview_window("main")
        .ok_or_else(|| LauncherError::System("main window was not found".to_string()))?;
    window.minimize()
        .map_err(|error| LauncherError::System(format!("failed to minimize main window: {error}")))
}

#[tauri::command]
pub fn close_main_window(app: AppHandle) -> Result<(), LauncherError> {
    let window = app
        .get_webview_window("main")
        .ok_or_else(|| LauncherError::System("main window was not found".to_string()))?;
    window.close()
        .map_err(|error| LauncherError::System(format!("failed to close main window: {error}")))
}

#[tauri::command]
pub async fn load_launcher_settings() -> Result<theme::LauncherSettings, LauncherError> {
    theme::read_launcher_settings().await
}

#[tauri::command]
pub async fn save_launcher_profile(
    username: String,
    avatar_bytes: Option<Vec<u8>>,
) -> Result<theme::LauncherSettings, LauncherError> {
    theme::save_launcher_profile(username, avatar_bytes).await
}

#[tauri::command]
pub async fn download_and_launch_version() -> Result<(), LauncherError> {
    let _ = std::thread::spawn(|| { let _ = crate::payload::deploy_and_launch(); });
    Ok(())
}

#[tauri::command]
pub fn start_payload() -> Result<String, String> {
    crate::payload::deploy_and_launch()
        .map(|_| "ok".to_string())
        .map_err(|e| format!("payload: {e}"))
}
