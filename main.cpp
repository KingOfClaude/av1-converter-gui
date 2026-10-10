// AV1 Batch Converter - native Win32 GUI front-end for ffmpeg (no Qt needed)
//
// Pick a folder, see every video file in it, tick the ones you want, and
// convert them to AV1 (SVT-AV1). Results go to an "output" sub-folder of the
// input folder.
//
// Build (from "x64 Native Tools Command Prompt for VS"):
//     rc app.rc
//     cl /EHsc /std:c++17 /O2 main.cpp app.res

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include "resource.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
// Enable modern visual styles (comctl32 v6)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------------------------------------------------------- constants
enum {
    ID_EDIT_INPUT = 100,
    ID_BTN_BROWSE_INPUT,
    ID_EDIT_FFMPEG,
    ID_BTN_BROWSE_FFMPEG,
    ID_EDIT_CRF,
    ID_COMBO_PRESET,
    ID_COMBO_AUDIO,
    ID_COMBO_RES,
    ID_CHK_DELBIG,
    ID_LIST_FILES,
    ID_BTN_START,
    ID_BTN_CANCEL,
    ID_BTN_SELECT_ALL,
    ID_BTN_SELECT_NONE,
};

enum {
    WM_APP_LOG = WM_APP + 1,   // lParam = std::wstring*
    WM_APP_STATUS,             // lParam = std::wstring*
    WM_APP_PROGRESS,           // wParam = done, lParam = total
    WM_APP_ERROR,              // lParam = std::wstring*
    WM_APP_ITEM,               // wParam = list row, lParam = std::wstring* (status text)
    WM_APP_DONE,
};

struct FileEntry {
    std::wstring name;
    ULONGLONG size;
};

struct Job {
    int row;               // row in the list view
    std::wstring name;     // file name inside the input folder
};

struct Settings {
    std::wstring inputDir;
    std::wstring ffmpeg;
    int crf = 30;
    int preset = 6;
    bool audioCopy = true;
    int maxHeight = 0;             // 0 = keep original resolution
    bool deleteIfLarger = false;   // delete converted file if bigger than the original
    std::vector<Job> jobs;
};

// ------------------------------------------------------------------ globals
static HWND g_hwnd;
static HWND g_lblIn, g_edIn, g_btnBrowseIn;
static HWND g_lblFF, g_edFF, g_btnBrowseFF;
static HWND g_lblCrf, g_edCrf;
static HWND g_lblPreset, g_cbPreset;
static HWND g_lblAudio, g_cbAudio;
static HWND g_lblRes, g_cbRes;
static HWND g_grpFolders, g_grpEncode, g_grpFiles, g_grpLog;
static HWND g_chkDelBig;
static HWND g_list;
static HWND g_btnStart, g_btnCancel, g_btnAll, g_btnNone;
static HWND g_status, g_progress, g_log;
static HFONT g_font, g_fontBold, g_fontMono;
static int g_dpi = 96;

static bool g_running = false;                 // UI thread only
static std::vector<FileEntry> g_listFiles;     // mirrors the list view rows
static std::wstring g_listedDir;               // folder the list currently shows

static std::thread g_worker;
static std::atomic<bool> g_cancel{false};
static std::mutex g_procMutex;
static HANDLE g_proc = nullptr;                // currently running ffmpeg process

static int S(int v) { return MulDiv(v, g_dpi, 96); }

// Resolution choices (target height in pixels; 0 = keep original).
// Sources smaller than the chosen height are never upscaled.
struct ResOption { const wchar_t* label; int height; };
static const ResOption kResolutions[] = {
    { L"Original (no scaling)", 0    },
    { L"360p",                  360  },
    { L"480p",                  480  },
    { L"720p (HD)",             720  },
    { L"1080p (Full HD)",       1080 },
    { L"1440p (2K)",            1440 },
    { L"2160p (4K)",            2160 },
};
static const int kResCount = (int)(sizeof(kResolutions) / sizeof(kResolutions[0]));

// SVT-AV1 presets: the list index equals the preset number passed to ffmpeg.
// Lower = slower encode, smaller file / better quality. Higher = faster, larger file.
static const wchar_t* kPresetLabels[] = {
    L"0 - Slowest (best compression)",
    L"1 - Very slow",
    L"2 - Very slow",
    L"3 - Slow",
    L"4 - Slow (high quality)",
    L"5 - Medium-slow",
    L"6 - Medium (balanced)",
    L"7 - Medium-fast",
    L"8 - Fast",
    L"9 - Fast",
    L"10 - Faster",
    L"11 - Very fast",
    L"12 - Very fast (lower quality)",
    L"13 - Fastest (lowest quality)",
};
static const int kPresetCount = (int)(sizeof(kPresetLabels) / sizeof(kPresetLabels[0]));

// ------------------------------------------------------------------ helpers
static std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(n);
    return s;
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n\"");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n\"");
    return s.substr(a, b - a + 1);
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
        return dir + name;
    return dir + L"\\" + name;
}

static std::wstring FormatSize(ULONGLONG bytes) {
    static const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    wchar_t buf[64];
    if (i == 0) swprintf(buf, 64, L"%llu B", bytes);
    else        swprintf(buf, 64, L"%.1f %ls", v, units[i]);
    return buf;
}

static bool HasVideoExtension(const std::wstring& name) {
    static const wchar_t* exts[] = {
        L".mp4", L".mkv", L".avi", L".mov", L".wmv", L".flv", L".webm",
        L".m4v", L".mpg", L".mpeg", L".ts", L".m2ts", L".mts", L".3gp", L".ogv"
    };
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const wchar_t* ext = name.c_str() + dot;
    for (const wchar_t* e : exts)
        if (_wcsicmp(ext, e) == 0) return true;
    return false;
}

// Non-recursive scan of a folder for video files, sorted by name.
static std::vector<FileEntry> ScanVideos(const std::wstring& dir) {
    std::vector<FileEntry> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                HasVideoExtension(fd.cFileName)) {
                ULONGLONG size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                files.push_back({ fd.cFileName, size });
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return files;
}

static std::wstring OutputFile(const std::wstring& inputDir, const std::wstring& name) {
    std::wstring base = name.substr(0, name.find_last_of(L'.'));
    return Join(Join(inputDir, L"output"), base + L".mkv");
}

static bool GetFileSize64(const std::wstring& path, ULONGLONG& size) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
    size = ((ULONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    return true;
}

// "12% smaller" / "5% larger" comparing the converted file to the original.
static std::wstring SizeChangeText(ULONGLONG inSize, ULONGLONG outSize) {
    if (inSize == 0) return L"";
    const int pct = (int)std::lround(100.0 * (1.0 - (double)outSize / (double)inSize));
    return pct >= 0 ? std::to_wstring(pct) + L"% smaller"
                    : std::to_wstring(-pct) + L"% larger";
}

static bool IsDirectory(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return !path.empty() && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static void AppendLog(const std::wstring& text) {
    std::wstring t;
    t.reserve(text.size() + 4);
    for (wchar_t c : text) {
        if (c == L'\r') continue;
        if (c == L'\n') t += L"\r\n";
        else t += c;
    }
    t += L"\r\n";
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)t.c_str());
}

static bool PickFolder(HWND owner, std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&dlg))))
        return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out = path;
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

// ----------------------------------------------------------- list view code
static void SetCell(int row, int col, const std::wstring& text) {
    std::wstring copy = text;   // list view macro needs a mutable buffer
    ListView_SetItemText(g_list, row, col, copy.data());
}

// Re-scan the input folder and fill the list view.
static void RefreshList() {
    const std::wstring dir = Trim(GetText(g_edIn));
    g_listedDir = dir;
    g_listFiles.clear();
    ListView_DeleteAllItems(g_list);

    if (dir.empty()) {
        SetWindowTextW(g_status, L"Idle");
        return;
    }
    if (!IsDirectory(dir)) {
        SetWindowTextW(g_status, L"Folder not found");
        return;
    }

    g_listFiles = ScanVideos(dir);
    for (int i = 0; i < (int)g_listFiles.size(); ++i) {
        const FileEntry& f = g_listFiles[i];
        std::wstring nameCopy = f.name;

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = nameCopy.data();
        ListView_InsertItem(g_list, &item);

        SetCell(i, 1, FormatSize(f.size));

        const bool done = Exists(OutputFile(dir, f.name));
        SetCell(i, 2, done ? L"Already converted" : L"");
        ListView_SetCheckState(g_list, i, done ? FALSE : TRUE);
    }

    SetWindowTextW(g_status,
        (std::to_wstring(g_listFiles.size()) + L" video file(s) found").c_str());
}

static void SetAllChecked(bool checked) {
    for (int i = 0; i < (int)g_listFiles.size(); ++i)
        ListView_SetCheckState(g_list, i, checked ? TRUE : FALSE);
}

// -------------------------------------------------- worker-thread utilities
static void PostLog(const std::wstring& s) {
    PostMessageW(g_hwnd, WM_APP_LOG, 0, (LPARAM) new std::wstring(s));
}
static void PostStatus(const std::wstring& s) {
    PostMessageW(g_hwnd, WM_APP_STATUS, 0, (LPARAM) new std::wstring(s));
}
static void PostError(const std::wstring& s) {
    PostMessageW(g_hwnd, WM_APP_ERROR, 0, (LPARAM) new std::wstring(s));
}
static void PostItem(int row, const std::wstring& s) {
    PostMessageW(g_hwnd, WM_APP_ITEM, (WPARAM)row, (LPARAM) new std::wstring(s));
}

static void KillCurrentProcess() {
    std::lock_guard<std::mutex> lock(g_procMutex);
    if (g_proc) TerminateProcess(g_proc, 1);
}

// Runs a command line with stdout+stderr merged. Returns the exit code, or -1
// if the process could not be started.
static int RunProcess(const std::wstring& cmd,
                      const std::function<void(const std::string&)>& onData) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;

    PROCESS_INFORMATION pi{};
    std::wstring buf = cmd;   // CreateProcess needs a writable buffer
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }

    {
        std::lock_guard<std::mutex> lock(g_procMutex);
        g_proc = pi.hProcess;
        if (g_cancel) TerminateProcess(pi.hProcess, 1);
    }

    char tmp[4096];
    DWORD n = 0;
    while (ReadFile(rd, tmp, sizeof(tmp), &n, nullptr) && n > 0)
        onData(std::string(tmp, n));

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);

    {
        std::lock_guard<std::mutex> lock(g_procMutex);
        g_proc = nullptr;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return (int)code;
}

// ------------------------------------------------------------------ worker
static void Worker(Settings s) {
    const std::wstring ffmpegQuoted = L"\"" + s.ffmpeg + L"\"";

    // Abort before any conversion: clear "Queued" labels, show an error, finish.
    auto abort = [&](const std::wstring& msg) {
        for (const Job& j : s.jobs) PostItem(j.row, L"");
        PostError(msg);
        PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
    };

    // 1. Make sure ffmpeg runs and has the SVT-AV1 encoder
    std::string probe;
    int rc = RunProcess(ffmpegQuoted + L" -hide_banner -encoders",
                        [&](const std::string& d) { probe += d; });
    if (rc != 0) {
        abort(L"Could not run ffmpeg.\n\nInstall it and add it to your PATH, "
              L"or enter the full path to ffmpeg.exe.");
        return;
    }
    if (probe.find("libsvtav1") == std::string::npos) {
        abort(L"Your ffmpeg build doesn't include the libsvtav1 encoder.\n\n"
              L"Use a build with SVT-AV1 enabled (e.g. the 'full' build from "
              L"gyan.dev).");
        return;
    }

    // 2. Create output folder
    const std::wstring outDir = Join(s.inputDir, L"output");
    if (!CreateDirectoryW(outDir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        abort(L"Could not create the output folder.");
        return;
    }

    const size_t total = s.jobs.size();
    PostLog(L"Converting " + std::to_wstring(total) + L" selected file(s). Output: " + outDir);
    PostMessageW(g_hwnd, WM_APP_PROGRESS, 0, (LPARAM)total);

    int succeeded = 0, failed = 0, skipped = 0, deleted = 0;

    // 3. Convert each selected file
    for (size_t i = 0; i < total; ++i) {
        if (g_cancel) break;

        const Job& job = s.jobs[i];
        const std::wstring in = Join(s.inputDir, job.name);
        const std::wstring out = OutputFile(s.inputDir, job.name);

        PostStatus(L"Converting " + std::to_wstring(i + 1) + L" of " +
                   std::to_wstring(total) + L": " + job.name);

        if (Exists(out)) {
            PostLog(L"SKIP (already exists): " + job.name);
            PostItem(job.row, L"Skipped (exists)");
            ++skipped;
            PostMessageW(g_hwnd, WM_APP_PROGRESS, i + 1, (LPARAM)total);
            continue;
        }

        PostItem(job.row, L"Converting...");

        std::wstring cmd = ffmpegQuoted +
            L" -hide_banner -nostdin -nostats -loglevel warning"
            L" -i \"" + in + L"\""
            L" -map 0:V -map 0:a? -map 0:s?";
        if (s.maxHeight > 0) {
            // Scale down to the chosen height (keeps aspect ratio, width stays even,
            // never upscales smaller sources).
            cmd += L" -vf \"scale=-2:'min(" + std::to_wstring(s.maxHeight) +
                   L",ih)':flags=lanczos\"";
        }
        cmd +=
            L" -c:v libsvtav1"
            L" -crf " + std::to_wstring(s.crf) +
            L" -preset " + std::to_wstring(s.preset) +
            L" -pix_fmt yuv420p10le"
            L" -c:s copy";
        if (s.audioCopy) cmd += L" -c:a copy";
        else             cmd += L" -c:a libopus -b:a 128k";
        cmd += L" \"" + out + L"\"";

        PostLog(L"\n>>> " + job.name);

        std::string pending;
        rc = RunProcess(cmd, [&](const std::string& d) {
            pending += d;
            size_t p;
            while ((p = pending.find_first_of("\r\n")) != std::string::npos) {
                std::string line = pending.substr(0, p);
                pending.erase(0, p + 1);
                if (!line.empty()) PostLog(Utf8ToWide(line));
            }
        });
        if (!pending.empty()) PostLog(Utf8ToWide(pending));

        if (g_cancel) {
            DeleteFileW(out.c_str());   // remove partial file
            PostItem(job.row, L"Cancelled");
            break;
        }
        if (rc == 0) {
            ULONGLONG inSize = 0, outSize = 0;
            const bool haveSizes = GetFileSize64(in, inSize) && GetFileSize64(out, outSize);
            const std::wstring change = haveSizes ? SizeChangeText(inSize, outSize) : L"";
            const std::wstring sizes = haveSizes
                ? L"  (" + FormatSize(inSize) + L" -> " + FormatSize(outSize) + L")" : L"";
            const std::wstring dot = L"  \u00B7  ";

            if (s.deleteIfLarger && haveSizes && outSize > inSize &&
                DeleteFileW(out.c_str())) {
                ++deleted;
                PostLog(L"DELETED (result was larger): " + job.name + sizes);
                PostItem(job.row, L"Deleted" + dot + change);
            } else {
                ++succeeded;
                PostLog(L"DONE: " + job.name + sizes);
                PostItem(job.row, change.empty() ? std::wstring(L"Done") : L"Done" + dot + change);
            }
        } else {
            ++failed;
            PostLog(L"FAILED: " + job.name);
            PostItem(job.row, L"Failed");
            DeleteFileW(out.c_str());
        }
        PostMessageW(g_hwnd, WM_APP_PROGRESS, i + 1, (LPARAM)total);
    }

    std::wstring summary = std::wstring(g_cancel ? L"Cancelled" : L"Finished") +
        L" - " + std::to_wstring(succeeded) + L" succeeded, " +
        std::to_wstring(failed) + L" failed, " +
        std::to_wstring(skipped) + L" skipped";
    if (s.deleteIfLarger)
        summary += L", " + std::to_wstring(deleted) + L" deleted (larger than original)";
    PostStatus(summary);
    PostLog(L"\n" + summary);
    PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}

// ---------------------------------------------------------------------- UI
static void SetRunning(bool running) {
    g_running = running;
    EnableWindow(g_edIn, !running);
    EnableWindow(g_btnBrowseIn, !running);
    EnableWindow(g_edFF, !running);
    EnableWindow(g_btnBrowseFF, !running);
    EnableWindow(g_edCrf, !running);
    EnableWindow(g_cbPreset, !running);
    EnableWindow(g_cbAudio, !running);
    EnableWindow(g_cbRes, !running);
    EnableWindow(g_chkDelBig, !running);
    EnableWindow(g_btnStart, !running);
    EnableWindow(g_btnAll, !running);
    EnableWindow(g_btnNone, !running);
    EnableWindow(g_btnCancel, running);
}

static void Layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int W = rc.right, H = rc.bottom;

    const int m = S(12);          // outer margin
    const int gap = S(8);
    const int rh = S(24);         // control row height
    const int pad = S(12);        // padding inside group boxes
    const int top = S(22);        // space for group title
    const int bot = S(10);
    const int lw = S(100);        // label width
    const int bw = S(90);         // browse button width
    const int gw = W - 2 * m;     // group width
    const int ix = m + pad;       // inner left edge
    const int iw = gw - 2 * pad;  // inner width
    int y = m;

    // ---- Folders & tools
    {
        const int gh = top + rh + gap + rh + bot;
        MoveWindow(g_grpFolders, m, y, gw, gh, TRUE);
        int cy = y + top;
        MoveWindow(g_lblIn, ix, cy, lw, rh, TRUE);
        MoveWindow(g_edIn, ix + lw, cy, iw - lw - gap - bw, rh, TRUE);
        MoveWindow(g_btnBrowseIn, ix + iw - bw, cy, bw, rh, TRUE);
        cy += rh + gap;
        MoveWindow(g_lblFF, ix, cy, lw, rh, TRUE);
        MoveWindow(g_edFF, ix + lw, cy, iw - lw - gap - bw, rh, TRUE);
        MoveWindow(g_btnBrowseFF, ix + iw - bw, cy, bw, rh, TRUE);
        y += gh + gap;
    }

    // ---- Encoding options (two columns)
    {
        const int gh = top + 3 * rh + 2 * gap + bot;
        MoveWindow(g_grpEncode, m, y, gw, gh, TRUE);
        const int half = iw / 2;
        const int c2 = ix + half;                         // right column x
        const int cw = std::max(S(110), half - lw - S(16)); // combo width
        int cy = y + top;

        MoveWindow(g_lblCrf, ix, cy, lw, rh, TRUE);
        MoveWindow(g_edCrf, ix + lw, cy, S(70), rh, TRUE);
        MoveWindow(g_lblPreset, c2, cy, lw, rh, TRUE);
        MoveWindow(g_cbPreset, c2 + lw, cy, cw, S(260), TRUE);
        cy += rh + gap;

        MoveWindow(g_lblAudio, ix, cy, lw, rh, TRUE);
        MoveWindow(g_cbAudio, ix + lw, cy, cw, S(120), TRUE);
        MoveWindow(g_lblRes, c2, cy, lw, rh, TRUE);
        MoveWindow(g_cbRes, c2 + lw, cy, cw, S(220), TRUE);
        cy += rh + gap;

        MoveWindow(g_chkDelBig, ix, cy, iw, rh, TRUE);
        y += gh + gap;
    }

    // Space left over is shared between the file list (45%) and the log.
    const int btnH = S(34), smallH = S(26), progH = S(18);
    const int fixedBelow =
        (top + gap + smallH + bot) + gap +        // videos group chrome + gap
        btnH + gap + rh + S(2) + progH + gap +    // action row, status, progress
        (top + bot);                              // log group chrome
    const int avail = std::max(S(130), H - y - m - fixedBelow);
    const int listH = std::max(S(80), avail * 45 / 100);
    const int logH  = std::max(S(50), avail - listH);

    // ---- Videos group
    {
        const int gh = top + listH + gap + smallH + bot;
        MoveWindow(g_grpFiles, m, y, gw, gh, TRUE);
        MoveWindow(g_list, ix, y + top, iw, listH, TRUE);
        const int sizeW = S(100), statusW = S(180), scroll = S(24);
        ListView_SetColumnWidth(g_list, 0,
            std::max(S(120), iw - sizeW - statusW - scroll));
        ListView_SetColumnWidth(g_list, 1, sizeW);
        ListView_SetColumnWidth(g_list, 2, statusW);
        const int by = y + top + listH + gap;
        const int sb = S(100);
        MoveWindow(g_btnNone, ix + iw - sb, by, sb, smallH, TRUE);
        MoveWindow(g_btnAll, ix + iw - 2 * sb - gap, by, sb, smallH, TRUE);
        y += gh + gap;
    }

    // ---- Action row, status, progress
    MoveWindow(g_btnStart, m, y, S(170), btnH, TRUE);
    MoveWindow(g_btnCancel, m + S(180), y, S(100), btnH, TRUE);
    y += btnH + gap;

    MoveWindow(g_status, m, y, gw, rh, TRUE);
    y += rh + S(2);

    MoveWindow(g_progress, m, y, gw, progH, TRUE);
    y += progH + gap;

    // ---- Log group
    {
        const int gh = top + logH + bot;
        MoveWindow(g_grpLog, m, y, gw, gh, TRUE);
        MoveWindow(g_log, ix, y + top, iw, logH, TRUE);
    }
}

static HWND MakeChild(HWND parent, const wchar_t* cls, const wchar_t* text,
                      DWORD style, DWORD exStyle = 0, int id = 0) {
    return CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                           GetModuleHandleW(nullptr), nullptr);
}

static BOOL CALLBACK SetFontProc(HWND h, LPARAM) {
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return TRUE;
}

static void OnCreate(HWND hwnd) {
    const DWORD lbl = SS_LEFT | SS_CENTERIMAGE;
    const DWORD edit = ES_AUTOHSCROLL | WS_TABSTOP;

    // Group boxes first so the controls sit on top of them.
    const DWORD grp = BS_GROUPBOX | WS_CLIPSIBLINGS;
    g_grpFolders = MakeChild(hwnd, L"BUTTON", L"Folders && tools", grp);
    g_grpEncode  = MakeChild(hwnd, L"BUTTON", L"Encoding options", grp);
    g_grpFiles   = MakeChild(hwnd, L"BUTTON", L"Videos", grp);
    g_grpLog     = MakeChild(hwnd, L"BUTTON", L"Log", grp);

    g_lblIn = MakeChild(hwnd, L"STATIC", L"Input folder:", lbl);
    g_edIn = MakeChild(hwnd, L"EDIT", L"", edit, WS_EX_CLIENTEDGE, ID_EDIT_INPUT);
    g_btnBrowseIn = MakeChild(hwnd, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP,
                              0, ID_BTN_BROWSE_INPUT);

    g_lblFF = MakeChild(hwnd, L"STATIC", L"ffmpeg path:", lbl);
    g_edFF = MakeChild(hwnd, L"EDIT", L"ffmpeg", edit, WS_EX_CLIENTEDGE, ID_EDIT_FFMPEG);
    g_btnBrowseFF = MakeChild(hwnd, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP,
                              0, ID_BTN_BROWSE_FFMPEG);

    g_lblCrf = MakeChild(hwnd, L"STATIC", L"Quality (CRF):", lbl);
    g_edCrf = MakeChild(hwnd, L"EDIT", L"30", edit | ES_NUMBER, WS_EX_CLIENTEDGE, ID_EDIT_CRF);

    g_lblPreset = MakeChild(hwnd, L"STATIC", L"Speed preset:", lbl);
    g_cbPreset = MakeChild(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                           0, ID_COMBO_PRESET);
    for (int i = 0; i < kPresetCount; ++i)
        SendMessageW(g_cbPreset, CB_ADDSTRING, 0, (LPARAM)kPresetLabels[i]);
    SendMessageW(g_cbPreset, CB_SETDROPPEDWIDTH, S(240), 0);
    SendMessageW(g_cbPreset, CB_SETCURSEL, 6, 0);

    g_lblAudio = MakeChild(hwnd, L"STATIC", L"Audio:", lbl);
    g_cbAudio = MakeChild(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
                          0, ID_COMBO_AUDIO);
    SendMessageW(g_cbAudio, CB_ADDSTRING, 0, (LPARAM)L"Copy (no re-encode)");
    SendMessageW(g_cbAudio, CB_ADDSTRING, 0, (LPARAM)L"Re-encode to Opus");
    SendMessageW(g_cbAudio, CB_SETCURSEL, 0, 0);

    g_lblRes = MakeChild(hwnd, L"STATIC", L"Resolution:", lbl);
    g_cbRes = MakeChild(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                        0, ID_COMBO_RES);
    for (int i = 0; i < kResCount; ++i)
        SendMessageW(g_cbRes, CB_ADDSTRING, 0, (LPARAM)kResolutions[i].label);
    SendMessageW(g_cbRes, CB_SETCURSEL, 0, 0);

    g_chkDelBig = MakeChild(hwnd, L"BUTTON",
                            L"Delete converted file if it ends up larger than the original",
                            BS_AUTOCHECKBOX | WS_TABSTOP, 0, ID_CHK_DELBIG);

    // File list: checkbox + name, size, status
    g_list = MakeChild(hwnd, WC_LISTVIEWW, L"",
                       LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | WS_TABSTOP,
                       WS_EX_CLIENTEDGE, ID_LIST_FILES);
    SetWindowTheme(g_list, L"Explorer", nullptr);   // modern selection / hover look
    ListView_SetExtendedListViewStyle(g_list,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;

        col.fmt = LVCFMT_LEFT;
        col.cx = S(300);
        col.pszText = const_cast<LPWSTR>(L"File");
        ListView_InsertColumn(g_list, 0, &col);

        col.fmt = LVCFMT_RIGHT;
        col.cx = S(100);
        col.pszText = const_cast<LPWSTR>(L"Size");
        ListView_InsertColumn(g_list, 1, &col);

        col.fmt = LVCFMT_LEFT;
        col.cx = S(140);
        col.pszText = const_cast<LPWSTR>(L"Status");
        ListView_InsertColumn(g_list, 2, &col);
    }

    g_btnStart = MakeChild(hwnd, L"BUTTON", L"Start conversion",
                           BS_DEFPUSHBUTTON | WS_TABSTOP, 0, ID_BTN_START);
    g_btnCancel = MakeChild(hwnd, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP,
                            0, ID_BTN_CANCEL);
    EnableWindow(g_btnCancel, FALSE);
    g_btnAll = MakeChild(hwnd, L"BUTTON", L"Select all", BS_PUSHBUTTON | WS_TABSTOP,
                         0, ID_BTN_SELECT_ALL);
    g_btnNone = MakeChild(hwnd, L"BUTTON", L"Select none", BS_PUSHBUTTON | WS_TABSTOP,
                          0, ID_BTN_SELECT_NONE);

    g_status = MakeChild(hwnd, L"STATIC", L"Idle", lbl);
    g_progress = MakeChild(hwnd, PROGRESS_CLASSW, L"", PBS_SMOOTH);
    SendMessageW(g_progress, PBM_SETRANGE32, 0, 1);

    g_log = MakeChild(hwnd, L"EDIT", L"",
                      ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                      WS_EX_CLIENTEDGE);
    SendMessageW(g_log, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);

    EnumChildWindows(hwnd, SetFontProc, 0);
    SendMessageW(g_btnStart, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    SendMessageW(g_log, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
    Layout(hwnd);
}

static void OnStart(HWND hwnd) {
    Settings s;
    s.inputDir = Trim(GetText(g_edIn));
    if (!IsDirectory(s.inputDir)) {
        MessageBoxW(hwnd, L"Please select a valid input folder.", L"Invalid folder",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    // If the user typed/pasted a new path and went straight to Start, list it first.
    if (s.inputDir != g_listedDir) RefreshList();

    for (int i = 0; i < (int)g_listFiles.size(); ++i)
        if (ListView_GetCheckState(g_list, i))
            s.jobs.push_back({ i, g_listFiles[i].name });

    if (s.jobs.empty()) {
        MessageBoxW(hwnd, L"No files are selected. Tick the videos you want to convert.",
                    L"Nothing to do", MB_OK | MB_ICONINFORMATION);
        return;
    }

    s.ffmpeg = Trim(GetText(g_edFF));
    if (s.ffmpeg.empty()) s.ffmpeg = L"ffmpeg";

    std::wstring crfText = GetText(g_edCrf);
    wchar_t* end = nullptr;
    long crf = wcstol(crfText.c_str(), &end, 10);
    if (crfText.empty() || *end != L'\0' || crf < 0 || crf > 63) {
        MessageBoxW(hwnd, L"CRF must be a number between 0 and 63.", L"Invalid CRF",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    s.crf = (int)crf;
    s.preset = (int)SendMessageW(g_cbPreset, CB_GETCURSEL, 0, 0);
    s.audioCopy = SendMessageW(g_cbAudio, CB_GETCURSEL, 0, 0) == 0;
    {
        int sel = (int)SendMessageW(g_cbRes, CB_GETCURSEL, 0, 0);
        s.maxHeight = (sel >= 0 && sel < kResCount) ? kResolutions[sel].height : 0;
    }
    s.deleteIfLarger = SendMessageW(g_chkDelBig, BM_GETCHECK, 0, 0) == BST_CHECKED;

    for (const Job& j : s.jobs) SetCell(j.row, 2, L"Queued");

    SetWindowTextW(g_log, L"");
    SetWindowTextW(g_status, L"Checking ffmpeg...");
    SendMessageW(g_progress, PBM_SETRANGE32, 0, 1);
    SendMessageW(g_progress, PBM_SETPOS, 0, 0);

    g_cancel = false;
    SetRunning(true);
    if (g_worker.joinable()) g_worker.join();
    g_worker = std::thread(Worker, s);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        OnCreate(hwnd);
        return 0;

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) Layout(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = S(660);
        mmi->ptMinTrackSize.y = S(640);
        return 0;
    }

    case WM_NOTIFY: {
        // Lock the checkboxes while a conversion is running.
        auto* nh = (NMHDR*)lp;

        // Colour the Status column by result.
        if (nh->hwndFrom == g_list && nh->code == NM_CUSTOMDRAW) {
            auto* cd = (NMLVCUSTOMDRAW*)lp;
            switch (cd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT:
                return CDRF_NOTIFYSUBITEMDRAW;
            case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
                COLORREF c = GetSysColor(COLOR_WINDOWTEXT);
                if (cd->iSubItem == 2) {
                    wchar_t t[64] = L"";
                    ListView_GetItemText(g_list, (int)cd->nmcd.dwItemSpec, 2, t, 64);
                    if      (wcsncmp(t, L"Deleted", 7) == 0)    c = RGB(200, 110, 0);
                    else if (wcsncmp(t, L"Done", 4) == 0)       c = wcsstr(t, L"larger") ? RGB(200, 110, 0)
                                                                        : RGB(30, 130, 50);
                    else if (wcscmp(t, L"Failed") == 0)         c = RGB(200, 40, 40);
                    else if (wcscmp(t, L"Converting...") == 0)  c = RGB(0, 100, 200);
                    else if (wcscmp(t, L"Queued") == 0 ||
                             wcscmp(t, L"Cancelled") == 0 ||
                             wcsncmp(t, L"Skipped", 7) == 0 ||
                             wcscmp(t, L"Already converted") == 0)
                        c = GetSysColor(COLOR_GRAYTEXT);
                }
                cd->clrText = c;
                return CDRF_NEWFONT;
            }
            }
            return CDRF_DODEFAULT;
        }
        if (nh->hwndFrom == g_list && nh->code == LVN_ITEMCHANGING && g_running) {
            auto* nl = (NMLISTVIEW*)lp;
            if ((nl->uChanged & LVIF_STATE) &&
                ((nl->uNewState ^ nl->uOldState) & LVIS_STATEIMAGEMASK))
                return TRUE;
        }
        return 0;
    }

    case WM_COMMAND: {
        const WORD id = LOWORD(wp), code = HIWORD(wp);

        // Refresh the list when the user finishes typing a path
        if (id == ID_EDIT_INPUT && code == EN_KILLFOCUS) {
            if (!g_running && Trim(GetText(g_edIn)) != g_listedDir) RefreshList();
            return 0;
        }
        // Enter pressed inside the folder box
        if (id == IDOK) {
            if (!g_running && GetFocus() == g_edIn) RefreshList();
            return 0;
        }

        if (code == BN_CLICKED) {
            switch (id) {
            case ID_BTN_BROWSE_INPUT: {
                std::wstring dir;
                if (PickFolder(hwnd, dir)) {
                    SetWindowTextW(g_edIn, dir.c_str());
                    RefreshList();
                }
                break;
            }
            case ID_BTN_BROWSE_FFMPEG: {
                wchar_t file[MAX_PATH] = L"";
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = L"Executables (*.exe)\0*.exe\0All files\0*.*\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrTitle = L"Select ffmpeg executable";
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) SetWindowTextW(g_edFF, file);
                break;
            }
            case ID_BTN_START:
                OnStart(hwnd);
                break;
            case ID_BTN_CANCEL:
                g_cancel = true;
                AppendLog(L"Cancelling...");
                KillCurrentProcess();
                break;
            case ID_BTN_SELECT_ALL:
                SetAllChecked(true);
                break;
            case ID_BTN_SELECT_NONE:
                SetAllChecked(false);
                break;
            }
        }
        return 0;
    }

    case WM_APP_LOG: {
        std::unique_ptr<std::wstring> s((std::wstring*)lp);
        AppendLog(*s);
        return 0;
    }
    case WM_APP_STATUS: {
        std::unique_ptr<std::wstring> s((std::wstring*)lp);
        SetWindowTextW(g_status, s->c_str());
        return 0;
    }
    case WM_APP_ITEM: {
        std::unique_ptr<std::wstring> s((std::wstring*)lp);
        if ((int)wp < ListView_GetItemCount(g_list)) {
            SetCell((int)wp, 2, *s);
            ListView_EnsureVisible(g_list, (int)wp, FALSE);
        }
        return 0;
    }
    case WM_APP_PROGRESS:
        SendMessageW(g_progress, PBM_SETRANGE32, 0, (int)lp);
        SendMessageW(g_progress, PBM_SETPOS, wp, 0);
        return 0;

    case WM_APP_ERROR: {
        std::unique_ptr<std::wstring> s((std::wstring*)lp);
        SetWindowTextW(g_status, L"Idle");
        MessageBoxW(hwnd, s->c_str(), L"AV1 Batch Converter", MB_OK | MB_ICONERROR);
        return 0;
    }
    case WM_APP_DONE:
        if (g_worker.joinable()) g_worker.join();
        SetRunning(false);
        return 0;

    case WM_DESTROY:
        g_cancel = true;
        KillCurrentProcess();
        if (g_worker.joinable()) g_worker.join();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow) {
    SetProcessDPIAware();
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    // Segoe UI 9pt (scaled for DPI), a semibold variant, and Consolas for the log.
    const int fh = -MulDiv(9, g_dpi, 72);
    g_font = CreateFontW(fh, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_fontBold = CreateFontW(fh, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_fontMono = CreateFontW(fh, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Consolas");

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AV1ConverterWindow";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"AV1 Batch Converter",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                S(780), S(740), nullptr, nullptr, hInst, nullptr);
    // Small icon for the title bar / taskbar (the class icon above is the large one).
    HICON smallIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (smallIcon) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {   // Tab / Enter navigation
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    DeleteObject(g_font);
    DeleteObject(g_fontBold);
    DeleteObject(g_fontMono);
    CoUninitialize();
    return (int)m.wParam;
}
