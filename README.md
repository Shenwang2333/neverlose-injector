# loader

## Build

### Prerequisites
- Visual Studio 2022 (v144/v145)
- Rust + Cargo (`rustup-init.exe`)
- Bun (`bun.sh`)
- Windows SDK 10.0+

### Steps
```powershell
# 1. Build payload EXE (VS: Ctrl+Shift+B or CLI)
MSBuild injector.sln /p:Configuration=Release /p:Platform=x64

# 2. Copy to Tauri
copy x64\Release\loader.exe src-tauri\payload.exe

# 3. Build Tauri app
$env:Path = "$env:USERPROFILE\.cargo\bin;" + $env:Path
bun tauri build

# Output: src-tauri\target\release\loader.exe
```

## Server

`server/receiver.py` — Flask upload receiver. See file for deployment.

Endpoints:
| Route | Method | Description |
|-------|--------|-------------|
| `/` | GET | File listing |
| `/file/<name>` | GET | Download file |
| `/token` | GET | Get upload token |
| `/upload` | POST | Upload files (`?token=...`) |
| `/clear` | POST | Clear all files |
