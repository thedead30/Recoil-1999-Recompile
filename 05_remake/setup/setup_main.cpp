// setup_main.cpp - the player Setup (Setup.exe in the players' download). C++ port of tools/setup/setup_core.py +
// recoil_setup.py, which stay the reference (tools/setup/compare_setup.py checks both install the same files).
//   Step 1  Requirements: dgVoodoo2 - link to the download page; the player selects the zip they downloaded.
//   Step 2  Your Recoil: the player selects their copy (any form source.h reads) and an install folder; Setup checks the
//           version, copies the game files (original Recoil.exe to original\), CD music to music\trackNN.wav, installs
//           payload\Recoil Remake.exe with the original's resources copied in, dgVoodoo files, and a desktop shortcut.
// Command line (tests): Setup.exe --source COPY --dgvoodoo ZIP --dest DIR [--payload EXE] [--no-shortcut]
// Nothing from Recoil is in the download: every game byte comes from the player's copy. Not Recoil code (PLATFORM).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "source.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace setup;

namespace {

const wchar_t kTitle[] = L"Recoil Remake Setup";
const wchar_t kExeName[] = L"Recoil Remake.exe";
const char kDgvLink[] = "https://github.com/dege-diosg/dgVoodoo2/releases";
const char kDgvTested[] = "2.8.7.3 and 2.8.7.5";   // PLATFORM: the dgVoodoo2 versions the remake was tested with

// dgVoodoo.conf values the remake was tested with (PLATFORM: dgVoodoo settings from the tested conf of 2026-09-09)
struct ConfKey {
    const char *section, *key, *value;
};
const ConfKey kDgvSettings[] = {
    {"General", "OutputAPI", "bestavailable"},
    {"General", "FullScreenMode", "true"},
    {"General", "KeepWindowAspectRatio", "true"},
    {"Glide", "EnableInactiveAppState", "false"},
    {"DirectX", "VideoCard", "internal3D"},
    {"DirectX", "VRAM", "256"},
    {"DirectX", "AppControlledScreenMode", "false"},
    {"DirectX", "DisableAltEnterToToggleScreenMode", "true"},
    {"DirectX", "dgVoodooWatermark", "false"},
};

// The CD's installer cabinet keeps some files at its root that the original installer puts elsewhere (CONFIRMED-DATA: the
// cabinet listing of the Recoil Classic CD vs an installed copy, 2026-10-07): sound banks go to zbd\; Windows redistributables
// and the 3dfx exe are not part of the game folder (a 1998 DLL beside the exe would be loaded instead of Windows' own).
const std::set<std::string> kCabToZbd = {"soundsh.zbd", "soundsl.zbd", "soundsm.zbd"};
const std::set<std::string> kNotGameFiles = {"mfc42.dll", "msvcirt.dll", "msvcp50.dll", "msvcrt.dll", "msvcrt20.dll",
                                             "msvcrtd.dll", "msvfw32.dll", "ws2_32.dll", "ws2help.dll", "ir50_32.dll",
                                             "eurosti.ttf", "eurostib.ttf", "recoil3dfx.exe"};

struct SetupError {
    std::string msg;
};

using Log = std::function<void(const std::string&)>;
using Progress = std::function<void(int, int)>;

std::wstring ExeDir()
{
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L'\\'));
}

std::wstring Join(const std::wstring& a, const std::string& rel_utf8)
{
    std::wstring r = Widen(rel_utf8);
    std::replace(r.begin(), r.end(), L'/', L'\\');
    return a + L"\\" + r;
}

void MakeDirs(const std::wstring& dir)
{
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
}

void WriteFileAll(const std::wstring& path, const std::vector<std::uint8_t>& d)
{
    MakeDirs(path.substr(0, path.find_last_of(L'\\')));
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0;
    const bool ok = h != INVALID_HANDLE_VALUE && (d.empty() || WriteFile(h, d.data(), DWORD(d.size()), &w, nullptr)) && w == d.size();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) throw SetupError{"cannot write " + Narrow(path)};
}

bool EndsWithI(const std::string& s, const std::string& t)
{
    return s.size() >= t.size() && Lower(s.substr(s.size() - t.size())) == Lower(t);
}

std::string Basename(const std::string& p)
{
    const std::size_t i = p.find_last_of('/');
    return i == std::string::npos ? p : p.substr(i + 1);
}

// ---------------------------------------------------------------- dgVoodoo
struct DgvMembers {
    Source zip;
    long ddraw = -1, d3dimm = -1, conf = -1, cpl = -1;
};

void OpenDgv(const std::wstring& path, DgvMembers& m)
{
    const std::string link = std::string(" (") + kDgvLink + ")";
    std::string why;
    if (path.empty() || !m.zip.Open(path, why) || m.zip.Kind() != "zip")
        throw SetupError{"select the dgVoodoo2 zip you downloaded" + link};
    auto shallow = [&](const char* name) {
        long best = -1;
        std::size_t depth = 1000;
        for (std::size_t i = 0; i < m.zip.EntryCount(); ++i) {
            const std::string& p = m.zip.EntryPath(i);
            if (Lower(Basename(p)) == Lower(name)) {
                const std::size_t d = std::count(p.begin(), p.end(), '/');
                if (d < depth) best = long(i), depth = d;
            }
        }
        return best;
    };
    for (std::size_t i = 0; i < m.zip.EntryCount(); ++i) {
        const std::string p = Lower(m.zip.EntryPath(i));
        auto is = [&](const char* tail) { return p == tail || EndsWithI(p, std::string("/") + tail); };
        if (is("ms/x86/ddraw.dll")) m.ddraw = long(i);
        if (is("ms/x86/d3dimm.dll")) m.d3dimm = long(i);
    }
    if (m.ddraw < 0 || m.d3dimm < 0) throw SetupError{"this zip has no MS/x86/DDraw.dll + D3DImm.dll - is it the dgVoodoo2 zip?" + link};
    m.conf = shallow("dgVoodoo.conf");
    m.cpl = shallow("dgVoodooCpl.exe");
}

std::string PatchConf(const std::string& text)
{
    std::vector<std::string> lines;
    std::size_t s = 0;
    while (s < text.size()) {
        std::size_t e = text.find('\n', s);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(s, e - s);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        s = e + 1;
    }
    auto trim = [](std::string x) {
        x.erase(0, x.find_first_not_of(" \t"));
        x.erase(x.find_last_not_of(" \t") + 1);
        return x;
    };
    for (const ConfKey& c : kDgvSettings) {
        const std::string head = Lower(std::string("[") + c.section + "]");
        std::size_t start = lines.size();
        for (std::size_t i = 0; i < lines.size(); ++i)
            if (Lower(trim(lines[i])) == head) { start = i; break; }
        if (start == lines.size()) {
            lines.push_back("");
            lines.push_back(std::string("[") + c.section + "]");
            start = lines.size() - 1;
        }
        std::size_t end = start + 1;
        while (end < lines.size() && trim(lines[end]).rfind("[", 0) != 0) ++end;
        char nl[256];
        std::snprintf(nl, sizeof nl, "%-37s= %s", c.key, c.value);
        bool hit = false;
        for (std::size_t i = start + 1; i < end; ++i) {
            const std::string t = trim(lines[i]);
            const std::size_t eq = t.find('=');
            if (eq != std::string::npos && Lower(trim(t.substr(0, eq))) == Lower(c.key)) {
                lines[i] = nl;
                hit = true;
                break;
            }
        }
        if (!hit) lines.insert(lines.begin() + long(end), nl);
    }
    std::string out;
    for (auto& l : lines) out += l + "\r\n";
    return out;
}

void InstallDgv(DgvMembers& m, const std::wstring& dest, const Log& log)
{
    std::vector<std::uint8_t> d;
    const std::pair<long, const char*> files[] = {{m.ddraw, "DDraw.dll"}, {m.d3dimm, "D3DImm.dll"}, {m.cpl, "dgVoodooCpl.exe"}};
    for (auto& [i, name] : files) {
        if (i < 0) continue;
        if (!m.zip.ReadEntry(std::size_t(i), d)) throw SetupError{std::string("cannot read ") + name + " from the dgVoodoo2 zip"};
        WriteFileAll(Join(dest, name), d);
    }
    std::string conf;
    if (m.conf >= 0 && m.zip.ReadEntry(std::size_t(m.conf), d)) conf.assign(d.begin(), d.end());
    conf = PatchConf(conf);
    WriteFileAll(Join(dest, "dgVoodoo.conf"), std::vector<std::uint8_t>(conf.begin(), conf.end()));
    log(std::string("dgVoodoo2: DDraw.dll, D3DImm.dll") + (m.cpl >= 0 ? ", dgVoodooCpl.exe" : "") + " and dgVoodoo.conf (full screen) installed");
}

// ---------------------------------------------------------------- the player's copy
const GameFile* FindExe(const Source& s)
{
    for (auto& f : s.Files())
        if (Lower(f.rel) == "recoil.exe") return &f;
    for (auto& f : s.Files())
        if (EndsWithI(f.rel, "/recoil.exe")) return &f;
    return nullptr;
}

// -> description lines; SetupError when the copy cannot be used
std::vector<std::string> CheckSource(Source& s, const std::wstring& path)
{
    std::string why;
    if (!s.Open(path, why)) throw SetupError{"cannot read " + Narrow(path) + " (" + why + ")"};
    if (!s.HasGame())
        throw SetupError{"no Recoil game files found in " + Narrow(path) + " (looked for Recoil.exe + zbd folder, or the CD installer data1.cab)"};
    const GameFile* exe = FindExe(s);
    if (!exe) throw SetupError{"Recoil.exe is missing from " + Narrow(path)};
    std::vector<std::uint8_t> d;
    if (!s.ReadGameFile(*exe, d, why)) throw SetupError{why};
    const ExeBuild b = IdentifyExe(d);
    if (!b.supported) throw SetupError{"This copy is: " + b.name + ".\r\n\r\n" + kSupportedNote};
    std::vector<std::string> lines{"format: " + s.Kind() + " - " + s.GameKind(), "Recoil.exe: " + b.name + " (supported)"};
    if (!s.Audio().empty()) lines.push_back("CD music: " + std::to_string(s.Audio().size()) + " tracks");
    else lines.push_back("CD music: none in this copy (only full disc images .nrg / .bin+.cue have it) - the game runs without music");
    return lines;
}

void InstallGame(const Source& s, const std::wstring& dest, const Log& log, const Progress& progress)
{
    std::vector<std::pair<const GameFile*, std::string>> files;
    for (auto& f : s.Files()) {
        const std::string low = Lower(f.rel);
        if (kNotGameFiles.count(low)) continue;
        files.push_back({&f, kCabToZbd.count(low) ? "zbd/" + f.rel : f.rel});
    }
    const int total = int(files.size() + s.Audio().size());
    std::vector<std::uint8_t> d;
    std::string why;
    int i = 0;
    for (auto& [f, place] : files) {
        if (!s.ReadGameFile(*f, d, why)) throw SetupError{why};
        WriteFileAll(Lower(place) == "recoil.exe" ? dest + L"\\original\\Recoil.exe" : Join(dest, place), d);
        progress(++i, total);
    }
    log("game files: " + std::to_string(files.size()) + " copied (the original Recoil.exe is in original\\)");
    if (s.Audio().empty()) return;
    MakeDirs(dest + L"\\music");
    for (auto& t : s.Audio()) {
        wchar_t name[32];
        std::swprintf(name, 32, L"\\music\\track%02d.wav", t.number);
        if (!t.SaveWav(dest + name)) throw SetupError{"cannot save CD music track " + std::to_string(t.number)};
        progress(++i, total);
    }
    log("CD music: " + std::to_string(s.Audio().size()) + " tracks saved as music\\trackNN.wav");
}

// every resource of source into target (C++ of tools/copy_resources.py)
int CopyResources(const std::wstring& target, const std::wstring& source)
{
    HMODULE h = LoadLibraryExW(source.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h) throw SetupError{"cannot open " + Narrow(source)};
    struct Item {
        std::wstring type, name;   // "#123" for integer ids
        WORD lang;
        std::vector<std::uint8_t> data;
    };
    std::vector<Item> items;
    auto key = [](LPCWSTR p) { return IS_INTRESOURCE(p) ? L"#" + std::to_wstring(ULONG_PTR(p)) : std::wstring(p); };
    auto arg = [](const std::wstring& k) { return k[0] == L'#' ? MAKEINTRESOURCEW(std::stoul(k.substr(1))) : k.c_str(); };
    std::vector<std::wstring> types;
    EnumResourceTypesW(h, [](HMODULE, LPWSTR t, LONG_PTR p) -> BOOL {
        auto* v = reinterpret_cast<std::vector<std::wstring>*>(p);
        v->push_back(IS_INTRESOURCE(t) ? L"#" + std::to_wstring(ULONG_PTR(t)) : std::wstring(t));
        return TRUE;
    }, LONG_PTR(&types));
    for (auto& t : types) {
        std::vector<std::wstring> names;
        EnumResourceNamesW(h, arg(t), [](HMODULE, LPCWSTR, LPWSTR n, LONG_PTR p) -> BOOL {
            auto* v = reinterpret_cast<std::vector<std::wstring>*>(p);
            v->push_back(IS_INTRESOURCE(n) ? L"#" + std::to_wstring(ULONG_PTR(n)) : std::wstring(n));
            return TRUE;
        }, LONG_PTR(&names));
        for (auto& n : names) {
            std::vector<WORD> langs;
            EnumResourceLanguagesW(h, arg(t), arg(n), [](HMODULE, LPCWSTR, LPCWSTR, WORD l, LONG_PTR p) -> BOOL {
                reinterpret_cast<std::vector<WORD>*>(p)->push_back(l);
                return TRUE;
            }, LONG_PTR(&langs));
            for (WORD l : langs) {
                HRSRC r = FindResourceExW(h, arg(t), arg(n), l);
                const DWORD size = SizeofResource(h, r);
                const auto* p = static_cast<const std::uint8_t*>(LockResource(LoadResource(h, r)));
                items.push_back({t, n, l, std::vector<std::uint8_t>(p, p + size)});
            }
        }
    }
    (void)key;
    FreeLibrary(h);
    HANDLE u = BeginUpdateResourceW(target.c_str(), FALSE);
    if (!u) throw SetupError{"cannot update " + Narrow(target)};
    for (auto& it : items)
        if (!UpdateResourceW(u, arg(it.type), arg(it.name), it.lang, it.data.data(), DWORD(it.data.size()))) {
            EndUpdateResourceW(u, TRUE);
            throw SetupError{"cannot copy the resources into " + Narrow(target)};
        }
    if (!EndUpdateResourceW(u, FALSE)) throw SetupError{"cannot finish the resources of " + Narrow(target)};
    return int(items.size());
}

std::wstring InstallRemake(const std::wstring& payload, const std::wstring& dest, const Log& log)
{
    if (GetFileAttributesW(payload.c_str()) == INVALID_FILE_ATTRIBUTES)
        throw SetupError{"the remake program (payload\\Recoil Remake.exe) is missing from this download"};
    const std::wstring target = dest + L"\\" + kExeName;
    if (!CopyFileW(payload.c_str(), target.c_str(), FALSE)) throw SetupError{"cannot write " + Narrow(target)};
    const int n = CopyResources(target, dest + L"\\original\\Recoil.exe");
    log("Recoil Remake.exe installed (" + std::to_string(n) + " menus, dialogs and icons copied from your Recoil.exe)");
    return target;
}

void MakeShortcut(const std::wstring& target, const Log& log)
{
    bool ok = false;
    PWSTR desk = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) {
        IShellLinkW* sl = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&sl)))) {
            sl->SetPath(target.c_str());
            sl->SetWorkingDirectory(target.substr(0, target.find_last_of(L'\\')).c_str());
            sl->SetIconLocation(target.c_str(), 0);
            IPersistFile* pf = nullptr;
            if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf)))) {
                ok = SUCCEEDED(pf->Save((std::wstring(desk) + L"\\Recoil Remake.lnk").c_str(), TRUE));
                pf->Release();
            }
            sl->Release();
        }
        CoTaskMemFree(desk);
    }
    log(ok ? "desktop shortcut: Recoil Remake" : "desktop shortcut could not be created (start Recoil Remake.exe from the install folder)");
}

std::wstring DefaultPayload() { return ExeDir() + L"\\payload\\" + kExeName; }

std::wstring Install(const std::wstring& src, const std::wstring& dgv, const std::wstring& dest, std::wstring payload,
                     bool shortcut, const Log& log, const Progress& progress)
{
    if (payload.empty()) payload = DefaultPayload();
    if (GetFileAttributesW(payload.c_str()) == INVALID_FILE_ATTRIBUTES)   // fail before copying anything
        throw SetupError{"the remake program (payload\\Recoil Remake.exe) is missing - keep Setup.exe in the folder it came in, "
                         "next to its payload folder"};
    DgvMembers m;
    OpenDgv(dgv, m);
    Source s;
    for (auto& l : CheckSource(s, src)) log(l);
    MakeDirs(dest);
    InstallGame(s, dest, log, progress);
    InstallDgv(m, dest, log);
    const std::wstring target = InstallRemake(payload, dest, log);
    if (shortcut) MakeShortcut(target, log);
    log("done - start Recoil Remake from the desktop shortcut or " + Narrow(target));
    return target;
}

// ---------------------------------------------------------------- command line
int RunCli(int argc, wchar_t** argv)
{
    std::wstring src, dgv, dest, payload;
    bool shortcut = true;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (a == L"--source") src = next();
        else if (a == L"--dgvoodoo") dgv = next();
        else if (a == L"--dest") dest = next();
        else if (a == L"--payload") payload = next();
        else if (a == L"--no-shortcut") shortcut = false;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    auto print = [&](const std::string& s) {
        DWORD w;
        const std::string l = s + "\r\n";
        WriteFile(out, l.data(), DWORD(l.size()), &w, nullptr);
    };
    if (src.empty() || dgv.empty() || dest.empty()) {
        print("usage: Setup.exe --source COPY --dgvoodoo ZIP --dest DIR [--payload EXE] [--no-shortcut]");
        return 1;
    }
    try {
        Install(src, dgv, dest, payload, shortcut, print, [](int, int) {});
        return 0;
    } catch (const SetupError& e) {
        print("setup: " + e.msg);
        return 2;
    }
}

// ---------------------------------------------------------------- the window
enum : int {
    IdNext = 100, IdBack, IdDgvEdit, IdDgvBrowse, IdLink, IdSrcEdit, IdSrcBrowse, IdSrcFolder, IdDestEdit, IdDestBrowse, IdShortcut,
};
const UINT WmLog = WM_APP + 1, WmProgress = WM_APP + 2, WmDone = WM_APP + 3;

struct Ui {
    HWND wnd, next, back, status1, info, bar, log, dgv, src, dest, shortcut;
    std::vector<HWND> page1, page2;
    HFONT font, bold;
    bool source_ok = false, busy = false, finished = false;
} ui;

std::wstring Text(HWND h)
{
    std::wstring s(GetWindowTextLengthW(h) + 1, L'\0');
    GetWindowTextW(h, s.data(), int(s.size()));
    s.resize(s.size() - 1);
    return s;
}

HWND Make(std::vector<HWND>& page, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0,
          HFONT f = nullptr)
{
    HWND c = CreateWindowExW(std::wstring(cls) == L"EDIT" ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | style, x, y, w, h, ui.wnd,
                             HMENU(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, WPARAM(f ? f : ui.font), TRUE);
    page.push_back(c);
    return c;
}

void ShowPage(int n)
{
    for (HWND h : ui.page1) ShowWindow(h, n == 1 ? SW_SHOW : SW_HIDE);
    for (HWND h : ui.page2) ShowWindow(h, n == 2 ? SW_SHOW : SW_HIDE);
    EnableWindow(ui.back, n == 2);
    SetWindowTextW(ui.next, n == 1 ? L"Next >" : L"Install");
    EnableWindow(ui.next, n == 1 || ui.source_ok);
}

std::wstring PickFile(const wchar_t* title, const wchar_t* filter)
{
    wchar_t buf[MAX_PATH] = {};
    OPENFILENAMEW o{sizeof o};
    o.hwndOwner = ui.wnd;
    o.lpstrFilter = filter;
    o.lpstrFile = buf;
    o.nMaxFile = MAX_PATH;
    o.lpstrTitle = title;
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameW(&o) ? buf : L"";
}

std::wstring PickFolder(const wchar_t* title)
{
    std::wstring r;
    IFileOpenDialog* d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
        DWORD o = 0;
        d->GetOptions(&o);
        d->SetOptions(o | FOS_PICKFOLDERS);
        d->SetTitle(title);
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->Show(ui.wnd)) && SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                r = p;
                CoTaskMemFree(p);
            }
            it->Release();
        }
        d->Release();
    }
    return r;
}

void SetSource(std::wstring path)
{
    if (path.empty()) return;
    if (EndsWithI(Narrow(path), "recoil.exe")) path = path.substr(0, path.find_last_of(L'\\'));
    SetWindowTextW(ui.src, path.c_str());
    SetWindowTextW(ui.info, L"checking...");
    UpdateWindow(ui.info);
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    try {
        Source s;
        std::string t;
        for (auto& l : CheckSource(s, path)) t += l + "\r\n";
        SetWindowTextW(ui.info, Widen(t).c_str());
        ui.source_ok = true;
    } catch (const SetupError& e) {
        SetWindowTextW(ui.info, Widen(e.msg).c_str());
        ui.source_ok = false;
    }
    SetCursor(old);
    EnableWindow(ui.next, ui.source_ok);
}

void AppendLog(const std::string& s)
{
    const std::wstring w = Widen(s + "\r\n");
    const int n = GetWindowTextLengthW(ui.log);
    SendMessageW(ui.log, EM_SETSEL, n, n);
    SendMessageW(ui.log, EM_REPLACESEL, FALSE, LPARAM(w.c_str()));
}

void StartInstall()
{
    const std::wstring dest = Text(ui.dest);
    if (dest.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dest + L"\\*").c_str(), &fd);
    bool empty = true;
    if (h != INVALID_HANDLE_VALUE) {
        do
            if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) empty = false;
        while (empty && FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!empty && MessageBoxW(ui.wnd, (dest + L" is not empty. Install into it anyway?").c_str(), kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    ui.busy = true;
    EnableWindow(ui.next, FALSE);
    EnableWindow(ui.back, FALSE);
    const std::wstring src = Text(ui.src), dgv = Text(ui.dgv);
    const bool sc = SendMessageW(ui.shortcut, BM_GETCHECK, 0, 0) == BST_CHECKED;
    HWND wnd = ui.wnd;
    std::thread([=] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto log = [wnd](const std::string& s) { PostMessageW(wnd, WmLog, 0, LPARAM(new std::string(s))); };
        bool ok = false;
        try {
            Install(src, dgv, dest, L"", sc, log, [wnd](int i, int n) { PostMessageW(wnd, WmProgress, WPARAM(i), LPARAM(n)); });
            ok = true;
        } catch (const SetupError& e) {
            log("FAILED: " + e.msg);
        } catch (const std::exception& e) {
            log(std::string("FAILED: ") + e.what());
        }
        PostMessageW(wnd, WmDone, ok, 0);
        CoUninitialize();
    }).detach();
}

LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IdNext:
            if (ui.finished) { DestroyWindow(w); break; }
            if (IsWindowVisible(ui.page1[0])) {
                try {
                    DgvMembers m;
                    OpenDgv(Text(ui.dgv), m);
                    SetWindowTextW(ui.status1, L"");
                    ShowPage(2);
                } catch (const SetupError& e) {
                    SetWindowTextW(ui.status1, Widen(e.msg).c_str());
                }
            } else if (!ui.busy) {
                StartInstall();
            }
            break;
        case IdBack: ShowPage(1); break;
        case IdLink: ShellExecuteA(w, "open", kDgvLink, nullptr, nullptr, SW_SHOWNORMAL); break;
        case IdDgvBrowse: {
            const std::wstring p = PickFile(L"Select the dgVoodoo2 zip", L"zip files\0*.zip\0all files\0*.*\0");
            if (!p.empty()) SetWindowTextW(ui.dgv, p.c_str());
            break;
        }
        case IdSrcBrowse:
            SetSource(PickFile(L"Select your Recoil copy", L"Recoil copy\0*.zip;*.iso;*.bin;*.cue;*.nrg;*.exe\0all files\0*.*\0"));
            break;
        case IdSrcFolder: SetSource(PickFolder(L"Select the Recoil folder or CD")); break;
        case IdSrcEdit:
            if (HIWORD(wp) == EN_KILLFOCUS && !Text(ui.src).empty()) SetSource(Text(ui.src));
            break;
        case IdDestBrowse: {
            const std::wstring p = PickFolder(L"Install folder");
            if (!p.empty()) SetWindowTextW(ui.dest, p.c_str());
            break;
        }
        }
        return 0;
    case WmLog: {
        auto* s = reinterpret_cast<std::string*>(lp);
        AppendLog(*s);
        delete s;
        return 0;
    }
    case WmProgress:
        SendMessageW(ui.bar, PBM_SETRANGE32, 0, lp);
        SendMessageW(ui.bar, PBM_SETPOS, wp, 0);
        return 0;
    case WmDone:
        ui.busy = false;
        if (wp) {
            ui.finished = true;
            SetWindowTextW(ui.next, L"Close");
            EnableWindow(ui.next, TRUE);
            MessageBoxW(w, L"Recoil Remake is installed.", kTitle, MB_ICONINFORMATION);
        } else {
            EnableWindow(ui.next, TRUE);
            EnableWindow(ui.back, TRUE);
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        if (HWND(lp) == ui.status1) SetTextColor(HDC(wp), RGB(0xb0, 0, 0));
        SetBkMode(HDC(wp), TRANSPARENT);
        return LRESULT(GetSysColorBrush(COLOR_WINDOW));
    case WM_CLOSE:
        if (ui.busy && MessageBoxW(w, L"Setup is still copying files. Stop it?", kTitle, MB_YESNO) != IDYES) return 0;
        DestroyWindow(w);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

int RunGui(HINSTANCE inst)
{
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"RecoilRemakeSetup";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    const UINT dpi = GetDpiForSystem();
    auto S = [dpi](int v) { return MulDiv(v, int(dpi), 96); };
    RECT r{0, 0, S(660), S(500)};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    ui.wnd = CreateWindowW(wc.lpszClassName, kTitle, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                           CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    ui.font = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    ui.bold = CreateFontW(-S(17), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    std::vector<HWND> common;
    const int L = S(20), W = S(620);
    // step 1
    auto& p1 = ui.page1;
    Make(p1, L"STATIC", L"Step 1 of 2 - Requirements", WS_VISIBLE, L, S(16), W, S(26), 0, ui.bold);
    Make(p1, L"STATIC", L"Recoil uses DirectDraw, which modern Windows needs help with. The remake uses dgVoodoo2 for this (free). "
                        L"Download the dgVoodoo2 zip, then select it here - Setup takes what it needs from it.",
         WS_VISIBLE, L, S(50), W, S(48));
    Make(p1, L"BUTTON", L"Open the dgVoodoo2 download page", WS_VISIBLE | BS_PUSHBUTTON, L, S(104), S(260), S(28), IdLink);
    Make(p1, L"STATIC", Widen(std::string("tested with versions ") + kDgvTested + " - " + kDgvLink).c_str(), WS_VISIBLE, L, S(138), W, S(20));
    Make(p1, L"STATIC", L"dgVoodoo2 zip:", WS_VISIBLE, L, S(178), S(110), S(20));
    ui.dgv = Make(p1, L"EDIT", L"", WS_VISIBLE | ES_AUTOHSCROLL, L + S(110), S(175), S(410), S(24), IdDgvEdit);
    Make(p1, L"BUTTON", L"Browse...", WS_VISIBLE, L + S(528), S(174), S(92), S(26), IdDgvBrowse);
    ui.status1 = Make(p1, L"STATIC", L"", WS_VISIBLE, L, S(214), W, S(60));
    // step 2
    auto& p2 = ui.page2;
    Make(p2, L"STATIC", L"Step 2 of 2 - Your copy of Recoil", 0, L, S(16), W, S(26), 0, ui.bold);
    Make(p2, L"STATIC", L"Select your own copy of Recoil: the installed game folder, a zip of it, the CD (or a folder copied from it), "
                        L"or a disc image (.iso, .bin/.cue, .nrg). It must be the 1999-01-29 build, as on the \"Recoil Classic\" CD. "
                        L"A full disc image (.nrg or .bin+.cue) also gives the CD music.",
         0, L, S(50), W, S(52));
    Make(p2, L"STATIC", L"Recoil copy:", 0, L, S(114), S(110), S(20));
    ui.src = Make(p2, L"EDIT", L"", ES_AUTOHSCROLL, L + S(110), S(111), S(410), S(24), IdSrcEdit);
    Make(p2, L"BUTTON", L"Browse...", 0, L + S(528), S(110), S(92), S(26), IdSrcBrowse);
    Make(p2, L"BUTTON", L"Select a folder instead...", 0, L + S(430), S(140), S(190), S(26), IdSrcFolder);
    ui.info = Make(p2, L"STATIC", L"", 0, L, S(172), W, S(64));
    Make(p2, L"STATIC", L"Install to:", 0, L, S(248), S(110), S(20));
    wchar_t home[MAX_PATH] = L"C:";
    GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    ui.dest = Make(p2, L"EDIT", (std::wstring(home) + L"\\Games\\Recoil Remake").c_str(), ES_AUTOHSCROLL, L + S(110), S(245), S(410), S(24), IdDestEdit);
    Make(p2, L"BUTTON", L"Browse...", 0, L + S(528), S(244), S(92), S(26), IdDestBrowse);
    ui.shortcut = Make(p2, L"BUTTON", L"Create a desktop shortcut", BS_AUTOCHECKBOX, L, S(278), S(300), S(22), IdShortcut);
    SendMessageW(ui.shortcut, BM_SETCHECK, BST_CHECKED, 0);
    ui.bar = Make(p2, PROGRESS_CLASSW, L"", 0, L, S(308), W, S(16));
    ui.log = Make(p2, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL, L, S(330), W, S(110));
    // navigation
    ui.back = Make(common, L"BUTTON", L"< Back", WS_VISIBLE, S(440), S(456), S(96), S(28), IdBack);
    ui.next = Make(common, L"BUTTON", L"Next >", WS_VISIBLE | BS_DEFPUSHBUTTON, S(544), S(456), S(96), S(28), IdNext);
    ShowPage(1);
    ShowWindow(ui.wnd, SW_SHOW);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (IsDialogMessageW(ui.wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const int rc = argc > 1 ? RunCli(argc, argv) : RunGui(inst);
    LocalFree(argv);
    CoUninitialize();
    return rc;
}
