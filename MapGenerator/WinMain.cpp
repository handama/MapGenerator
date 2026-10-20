#include "pch.h"
#include <windows.h> // 需要包含 Win32 API 头文件
#include <commctrl.h> // 进度条控件（PROGRESS_CLASS）
#include <shlobj.h>  // SHBrowseForFolder 选择输出文件夹
#include <strsafe.h>
#include "MapGen.h"
#include <cstring>  // memcpy

// CreateFontW/DeleteObject/SetBkColor在gdi32.lib，显式链接保险
#pragma comment(lib, "gdi32.lib")
// 进度条控件在comctl32.lib
#pragma comment(lib, "comctl32.lib")
// SHBrowseForFolder / SHGetPathFromIDList 在 shell32，CoTaskMemFree / CoInitializeEx 在 ole32
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

// 控件ID
#define ID_GENERATE_BTN 101
#define ID_CLEAR_BTN    102
#define ID_PROGRESS_BAR 120
#define ID_OUTDIR_EDIT  121   // 输出目录显示框（只读，显示当前输出文件夹）
#define ID_BROWSE_BTN   122   // "浏览…"按钮：弹文件夹选择对话框

#define ID_ENV_COMBO     110
#define ID_TIME_COMBO    111
#define ID_THEATER_COMBO 112
#define ID_SIZE_COMBO    113
#define ID_ORE_COMBO     114
#define ID_PLAYER_COMBO  115
#define ID_GLOBALSEED_EDIT 116   // 全局种子输入框
#define ID_MAPSEED_EDIT    117   // 地形主体种子输入框
#define ID_GAMETYPE_COMBO  118   // 地图类型：0单人(.map) 1多人(.yrm)

// 玩家可调整的生成参数（除玩家数外均为下拉框选中项索引）
struct MapGenParams
{
    int environment;  // 环境：0群岛 1大岛屿 2大岛屿群 3内陆 4山地
    int timeOfDay;    // 时间：0早晨 1下午 2黄昏 3夜晚
    int theater;      // 剧场：0温带 1雪地
    int mapSize;       // 地图大小：0小 1中 2大 3特大
    int oreDensity;   // 资源：0低 1中 2高 3极高
    int playerIndex;  // 玩家数下拉框索引：0对应2人，6对应8人
    int gameType;     // 地图类型：0单人(.map) 1多人(.yrm)
    unsigned int globalSeedInput;  // 全局种子框的输入：0=每次随机
    unsigned int mapSeedInput;     // 地形主体种子框的输入：0=原版默认(0)

    int PlayerCount() const { return playerIndex + 2; }
};

static const wchar_t* kEnvOptions[]     = { L"群岛", L"大岛屿", L"大岛屿群", L"内陆", L"山地" };
static const wchar_t* kTimeOptions[]    = { L"早晨", L"下午", L"黄昏", L"夜晚" };
static const wchar_t* kTheaterOptions[] = { L"温带", L"雪地" };
static const wchar_t* kSizeOptions[]    = { L"小", L"中", L"大", L"特大" };
static const wchar_t* kOreOptions[]     = { L"低", L"中", L"高", L"极高" };
static const wchar_t* kPlayerOptions[]  = { L"2", L"3", L"4", L"5", L"6", L"7", L"8" };
static const wchar_t* kGameTypeOptions[] = { L"单人地图(.map)", L"多人地图(.yrm)" };

// 每一行参数UI：标签 + 选项列表 + 下拉框控件ID
struct ParamRow
{
    const wchar_t* label;
    const wchar_t* const* options;
    int optionCount;
    int comboId;
};

static const ParamRow kParamRows[] =
{
    { L"环境",     kEnvOptions,     5, ID_ENV_COMBO },
    { L"时间",     kTimeOptions,    4, ID_TIME_COMBO },
    { L"剧场",     kTheaterOptions, 2, ID_THEATER_COMBO },
    { L"地图大小", kSizeOptions,    4, ID_SIZE_COMBO },
    { L"资源",     kOreOptions,     4, ID_ORE_COMBO },
    { L"玩家数",   kPlayerOptions,  7, ID_PLAYER_COMBO },
    { L"地图类型", kGameTypeOptions, 2, ID_GAMETYPE_COMBO },
};

static HFONT g_uiFont = nullptr;

// ---- 右侧雷达底图预览区 ----------------------------------------------------
// 左侧留给参数控件（宽 400），右边这块画 "RMG: Compute Radar Image" 生成的雷达图。
static const int kPanelLeft = 400;
static HBITMAP g_radarBitmap = nullptr;   // 32 位自上而下 DIB，走 StretchBlt
static int     g_radarWidth = 0;
static int     g_radarHeight = 0;

// 预览面板的矩形：左边 400px 留给参数控件，其余是面板，四周内缩一圈。
static const int kPanelTop   = 22;
static const int kPanelInset = 16;

// 面板内部各行的纵向排布（相对 panel.top），绘制与控件布局共用同一组值
static const int kTitleHeight  = 22;
static const int kStatusHeight = 20;
static const int kBarTop       = 44;
static const int kBarHeight    = 18;
static const int kViewTop      = 66;

static RECT PreviewPanelRect(HWND hWnd)
{
    RECT client = {};
    GetClientRect(hWnd, &client);
    RECT panel = { kPanelLeft, kPanelTop,
                   client.right - kPanelInset, client.bottom - kPanelInset };
    return panel;
}

// 把雷达图的像素搬进一张 DIB（0xAARRGGBB，正好是 32bpp DIB 的字节序）。
static void SetRadarBitmap(const RadarImage& image)
{
    if (g_radarBitmap)
    {
        DeleteObject(g_radarBitmap);
        g_radarBitmap = nullptr;
    }
    g_radarWidth = 0;
    g_radarHeight = 0;

    if (image.Empty())
        return;

    BITMAPINFO info = {};
    info.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth       = image.width;
    info.bmiHeader.biHeight      = -image.height;   // 负高度 = 自上而下
    info.bmiHeader.biPlanes      = 1;
    info.bmiHeader.biBitCount    = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr)
    {
        if (dib)
            DeleteObject(dib);
        return;
    }

    memcpy(bits, image.pixels.data(),
           static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4);
    g_radarBitmap = dib;
    g_radarWidth = image.width;
    g_radarHeight = image.height;
}

// ---- 进度阶梯（对应 vanilla 的 sub_643C50 tick）----------------------------
// 语义与出处见 MapGenDone.cpp：主流程在固定检查点上报 55 / 60 / 65 / 70 / 75 /
// 80 / 85 / 90 / 95 %，最后一次 100% 由 Done 发出。生成期间每报一次就同步重画
// 预览面板，把百分比当场显示出来（原版用消息泵，端口用 UpdateWindow，避免重入）。
static int  g_progressPercent = -1;   // -1 = 还没生成过
static HWND g_progressWindow  = nullptr;
static HWND g_progressBar     = nullptr;   // 进度条控件（PROGRESS_CLASS）

static void OnProgress(void* /*context*/, int percent)
{
    g_progressPercent = percent;
    if (g_progressBar)
        SendMessageW(g_progressBar, PBM_SETPOS, (WPARAM)percent, 0);

    if (g_progressWindow)
    {
        InvalidateRect(g_progressWindow, nullptr, FALSE);
        UpdateWindow(g_progressWindow);
    }
}

// 窗口尺寸变化时把进度条摆回面板里（其余控件位置固定，只有它跟着面板走）。
static void LayoutPreviewPanel(HWND hWnd)
{
    if (g_progressBar == nullptr)
        return;

    const RECT panel = PreviewPanelRect(hWnd);
    if (panel.right - 2 <= panel.left + 2 || panel.bottom <= panel.top + kViewTop)
        return;

    SetWindowPos(g_progressBar, nullptr,
                 panel.left + 2, panel.top + kBarTop,
                 (panel.right - 2) - (panel.left + 2), kBarHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

// 创建控件并统一套用字体
static HWND CreateCtl(HWND parent, const wchar_t* className, const wchar_t* text,
                      DWORD style, int x, int y, int w, int h, int id, HINSTANCE inst)
{
    HWND ctl = CreateWindowW(className, text, style, x, y, w, h,
                             parent, (HMENU)(INT_PTR)id, inst, nullptr);
    if (ctl && g_uiFont)
        SendMessageW(ctl, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    return ctl;
}

// 读取下拉框当前选中索引（异常情况按第0项处理）
static int ComboSel(HWND hWnd, int comboId)
{
    int sel = (int)SendDlgItemMessageW(hWnd, comboId, CB_GETCURSEL, 0, 0);
    return (sel == CB_ERR) ? 0 : sel;
}

// 读取种子输入框：只接受十进制数字，空串或非数字按 0 处理
static unsigned int SeedEditValue(HWND hWnd, int editId)
{
    wchar_t buf[16] = {};
    GetDlgItemTextW(hWnd, editId, buf, ARRAYSIZE(buf));
    return (unsigned int)wcstoul(buf, nullptr, 10);
}

// 从UI读取全部生成参数，之后生成器直接消费这个结构体即可
static MapGenParams ReadParams(HWND hWnd)
{
    MapGenParams p = {};
    p.environment = ComboSel(hWnd, ID_ENV_COMBO);
    p.timeOfDay   = ComboSel(hWnd, ID_TIME_COMBO);
    p.theater     = ComboSel(hWnd, ID_THEATER_COMBO);
    p.mapSize     = ComboSel(hWnd, ID_SIZE_COMBO);
    p.oreDensity  = ComboSel(hWnd, ID_ORE_COMBO);
    p.playerIndex = ComboSel(hWnd, ID_PLAYER_COMBO);
    p.gameType    = ComboSel(hWnd, ID_GAMETYPE_COMBO);
    p.globalSeedInput = SeedEditValue(hWnd, ID_GLOBALSEED_EDIT);
    p.mapSeedInput    = SeedEditValue(hWnd, ID_MAPSEED_EDIT);
    return p;
}

// 将 UI 的 environment 索引映射为 RA2 的 LandType。
// 加回群岛后 UI 顺序与 LandType 枚举值一一对应：
// UI: 0=群岛, 1=大岛屿, 2=大岛屿群, 3=内陆, 4=山地
// LandType: 0=Archipelago, 1=Continent, 2=TeamContinent, 3=Inland, 4=Mountainous
static RmLandType EnvToLandType(int envIndex)
{
    switch (envIndex)
    {
    case 0: return RmLandType::Archipelago;
    case 1: return RmLandType::Continent;
    case 2: return RmLandType::TeamContinent;
    case 3: return RmLandType::Inland;
    case 4: return RmLandType::Mountainous;
    default: return RmLandType::Continent;
    }
}

static const wchar_t* LandTypeStr(RmLandType lt)
{
    switch (lt)
    {
    case RmLandType::Archipelago:   return L"群岛";
    case RmLandType::Continent:     return L"大岛屿";
    case RmLandType::TeamContinent: return L"大岛屿群";
    case RmLandType::Inland:        return L"内陆";
    case RmLandType::Mountainous:   return L"山地";
    default: return L"未知";
    }
}

// 取得 exe 所在文件夹（带结尾反斜杠）。这也是输出目录的默认值：地图、雷达图
// 和日志默认直接落在 exe 旁边。
static void GetExeDir(wchar_t* dir, size_t dirCch)
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash)
        *(slash + 1) = 0;
    else
        exePath[0] = 0;
    StringCchCopyW(dir, dirCch, exePath);
}

// 读取界面上当前选定的输出目录：直接取输出目录框里的路径并补上结尾反斜杠；
// 万一框是空的，回退到 exe 所在文件夹。
static void GetChosenOutputDir(HWND hWnd, wchar_t* dir, size_t dirCch)
{
    wchar_t buf[MAX_PATH] = {};
    GetDlgItemTextW(hWnd, ID_OUTDIR_EDIT, buf, ARRAYSIZE(buf));
    if (buf[0] == 0)
    {
        GetExeDir(dir, dirCch);
        return;
    }
    StringCchCopyW(dir, dirCch, buf);
    const size_t len = wcslen(dir);
    if (len > 0 && dir[len - 1] != L'\\' && dir[len - 1] != L'/')
        StringCchCatW(dir, dirCch, L"\\");
}

// 文件夹选择对话框刚弹出来时，把定位点设成当前输出目录，省得每次从根目录翻。
// BFFCALLBACK 的签名是 (HWND, UINT, LPARAM, LPARAM)，第三个参数本回调不用。
static int CALLBACK BrowseFolderCallback(HWND dlg, UINT msg,
                                         LPARAM /*lp*/, LPARAM lpData)
{
    if (msg == BFFM_INITIALIZED)
        SendMessageW(dlg, BFFM_SETSELECTIONW, TRUE, lpData);
    return 0;
}

// "浏览…"按钮：弹系统自带的"选择文件夹"对话框，选中后把路径填进输出目录框。
static void OnBrowseOutputDir(HWND hWnd)
{
    wchar_t current[MAX_PATH] = {};
    GetDlgItemTextW(hWnd, ID_OUTDIR_EDIT, current, ARRAYSIZE(current));

    BROWSEINFOW bi = {};
    bi.hwndOwner = hWnd;
    bi.lpszTitle = L"选择地图输出文件夹";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn      = BrowseFolderCallback;
    bi.lParam    = reinterpret_cast<LPARAM>(current);

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl != nullptr)
    {
        wchar_t path[MAX_PATH] = {};
        if (SHGetPathFromIDListW(pidl, path))
            SetDlgItemTextW(hWnd, ID_OUTDIR_EDIT, path);
        CoTaskMemFree(pidl);
    }
}

// 删除指定目录里符合通配符的所有文件（不进子目录），返回成功删除的个数
static int DeleteFilesByPattern(const wchar_t* dir, const wchar_t* pattern)
{
    wchar_t spec[MAX_PATH];
    StringCchPrintfW(spec, ARRAYSIZE(spec), L"%s%s", dir, pattern);

    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(spec, &fd);
    if (hf == INVALID_HANDLE_VALUE)
        return 0;

    int removed = 0;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        wchar_t full[MAX_PATH];
        StringCchPrintfW(full, ARRAYSIZE(full), L"%s%s", dir, fd.cFileName);
        if (DeleteFileW(full))
            ++removed;
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    return removed;
}

// "清除输出"按钮：确认后删掉当前输出文件夹下所有 .map / .yrm 和
// .isopack5(.txt) 文件。雷达图 radar_preview.png、seed_history.txt、
// rmg_diag.log 不在删除范围内。
static void OnClearOutputs(HWND hWnd)
{
    wchar_t dir[MAX_PATH];
    GetChosenOutputDir(hWnd, dir, ARRAYSIZE(dir));

    const int answer = MessageBoxW(hWnd,
        L"确定要删除所选输出文件夹下所有 .map/.yrm 地图文件和 .isopack5 阶段快照吗？\n\n"
        L"雷达图、种子记录、日志不会被删。",
        L"确认清除", MB_YESNO | MB_ICONQUESTION);
    if (answer != IDYES)
        return;

    CreateDirectoryW(dir, nullptr);          // 目录不存在就建，删 0 个也无妨

    const int maps   = DeleteFilesByPattern(dir, L"*.map")
                     + DeleteFilesByPattern(dir, L"*.yrm");
    const int packs1 = DeleteFilesByPattern(dir, L"*.isopack5.txt");
    const int packs2 = DeleteFilesByPattern(dir, L"*.isopack5");  // 无 .txt 后缀的兜底

    wchar_t msg[160];
    StringCchPrintfW(msg, ARRAYSIZE(msg),
                     L"已删除地图文件 %d 个、阶段快照文件 %d 个。",
                     maps, packs1 + packs2);
    MessageBoxW(hWnd, msg, L"清除完成", MB_OK | MB_ICONINFORMATION);
}

// 把本轮实际使用的种子追加到输出目录下的 seed_history.txt（UTF-8 带 BOM）。
// 每成功输出一张地图记一行，事后想复现哪张图，把行里的种子填回界面即可。
static void AppendSeedHistory(const wchar_t* outDir, const SYSTEMTIME& now,
                              const wchar_t* envName, const wchar_t* mapFileName,
                              unsigned int globalSeed, bool globalFromInput,
                              unsigned int mapSeed, bool mapFromInput)
{
    wchar_t lineW[512];
    StringCchPrintfW(lineW, ARRAYSIZE(lineW),
        L"%04d-%02d-%02d %02d:%02d:%02d  地形=%s  文件=%s  "
        L"全局种子=%u(0x%08X,%s)  地形种子=%u(%s)\r\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        envName, mapFileName,
        globalSeed, globalSeed, globalFromInput ? L"手填" : L"随机",
        mapSeed, mapFromInput ? L"手填" : L"默认");

    char lineUtf8[1024];
    const int n = WideCharToMultiByte(CP_UTF8, 0, lineW, -1,
                                      lineUtf8, sizeof(lineUtf8), nullptr, nullptr);
    if (n <= 0)
        return;

    wchar_t histPath[MAX_PATH];
    StringCchPrintfW(histPath, ARRAYSIZE(histPath), L"%sseed_history.txt", outDir);

    const DWORD want = static_cast<DWORD>(n - 1);   // 不含结尾 '\0'
    DWORD attr = GetFileAttributesW(histPath);
    const bool fileExists = (attr != INVALID_FILE_ATTRIBUTES)
                            && !(attr & FILE_ATTRIBUTE_DIRECTORY);

    HANDLE hf = CreateFileW(histPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE)
        return;

    // 新文件先写 UTF-8 BOM，记事本/FA2 打开才不会按本地码页读中文。
    if (!fileExists)
    {
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        DWORD written = 0;
        WriteFile(hf, bom, 3, &written, nullptr);
    }
    DWORD written = 0;
    WriteFile(hf, lineUtf8, want, &written, nullptr);
    CloseHandle(hf);
}

// 点击"生成"：读取参数 → 调用地形生成主体 → 回显结果
static void OnGenerate(HWND hWnd)
{
    MapGenParams p = ReadParams(hWnd);

    // 本轮输出目录：界面输出目录框里的文件夹（默认 exe 所在文件夹）。
    // 地图、雷达图、种子记录和日志全部写这里，构造完生成器立刻同步给它。
    wchar_t outDir[MAX_PATH] = {};
    GetChosenOutputDir(hWnd, outDir, ARRAYSIZE(outDir));

    // 生成按钮在生成期间禁用（vanilla 的 sub_596300 也这么干：
    // 0x5963d4 EnableWindow(hwnd, FALSE)），结束时恢复。
    HWND generateButton = GetDlgItem(hWnd, ID_GENERATE_BTN);
    if (generateButton)
        EnableWindow(generateButton, FALSE);

    // 这一轮写出的 .map 路径（落盘成功后填上，供结果框显示）
    wchar_t mapPath[MAX_PATH] = {};

    // 组装生成配置（对应 RandomMapGenerator 关键字段）
    MapGenConfig cfg = {};
    cfg.landType    = EnvToLandType(p.environment);
    cfg.theater     = p.theater;
    cfg.timeOfDay   = p.timeOfDay;
    cfg.sizeSlider  = p.mapSize;     // 尺寸滑块 0-3
    cfg.playerCount = p.PlayerCount(); // 玩家数 2-8
    cfg.oreDensity  = p.oreDensity;
    // 地图类型：单人 = .map（[Basic] 有 Player，[Countries]+[Houses] 登记
    // "Neutral House"），多人 = .yrm（无 Player，[Header] 带地图矩形和
    // Waypoint1..8，[Houses] 登记 rulesmd.ini [Countries] 的整份国家表）。
    // 两套写法的出处见 MapGenMapFile.cpp 的 SaveMapFile。
    cfg.multiplayer = (p.gameType == 1);

    // 两台随机器的种子都从界面的输入框取：
    //   全局种子（对应引擎 this_pRandomizer @0x886B88）：水量、坡道密度等全局
    //     选项由它掷出。框里填 0 = 每轮用 GetTickCount() 现取（默认，每张图不同）；
    //     填非 0 值 = 按填的值来，两张图完全一致、可复现。
    //   地形种子（地图自身 RNG dword_ABE890）：原版恒为 0（0x58b770）。框里填 0
    //     = 保持原版行为；填非 0 值 = 连地形主体一起换序列，用于调试。
    const uint32_t globalSeedInput = p.globalSeedInput;
    const uint32_t mapSeedInput    = p.mapSeedInput;
    const uint32_t globalSeed = (globalSeedInput != 0)
                                  ? globalSeedInput
                                  : (uint32_t)GetTickCount();
    RandomMapGenerator rmg;
    // 输出目录必须在第一条诊断日志之前同步好，rmg_diag.log 才会落在所选
    // 文件夹里（SetOutputDir 同时设置 DiagLog 用的静态目录）。
    rmg.SetOutputDir(outDir);

    RandomMapGenerator::DiagLog("GLOBAL-SEED %08X source=%s MAP-SEED %u source=%s",
                                globalSeed, globalSeedInput ? "input" : "tickcount",
                                mapSeedInput, mapSeedInput ? "input" : "default0");

    // 把进度阶梯接到预览面板上。Done() 结束时会把这根线摘掉（对应原版
    // sub_643E70 清掉进度条的 hWnd），所以每轮生成前重新挂一次。
    g_progressWindow = hWnd;
    g_progressPercent = 0;
    if (g_progressBar)
        SendMessageW(g_progressBar, PBM_SETPOS, 0, 0);   // 进度条归零
    rmg.SetProgressSink(OnProgress, nullptr);

    cfg.global = rmg.RollGlobalOptions(globalSeed);
    cfg.randomSeed = (uint32_t)cfg.global.seed04C;
    cfg.mapRngSeed = mapSeedInput;   // 0 = 原版固定种子 0

    // 水量 this[19]（对应 sub_597260 @ 0x597282）：
    //   this[19] = RandomRanged(0x82B0A8[地形], 0x82B0BC[地形])
    // 这就是 RollGlobalOptions 里同一台全局随机器（this_pRandomizer）按同一
    // 顺序掷出的那一项，直接取用——不再另用 CRT rand 重掷一份（那会用另一台
    // 随机数源造出并非原版的值）。表：群岛 [75,100] 大岛屿 [0,25]
    // 大岛屿群 [50,100] 内陆/山地 [0,100]。原版在对话框参数变化时会重掷
    // （sub_596C70 @ 0x596E08 → sub_597260），但"生成"时用的是最后一次掷出的值。
    // 它与生成主 RNG dst_ 无关，不消耗 dst_ 的随机序列。
    // roll 出 0 时内陆/山地走"全陆地"分支（对应原版 this[19]==0 跳过 sub_59C580）。
    cfg.waterAmount = cfg.global.waterAmount;

    // 调用地形生成主体（对应 sub_599650）
    bool ok = rmg.GenerateMapBody(cfg);

    // 地形生成分派（对应 sub_598960 @ 0x598aed - 0x598b0d）：
    //   LandType 3/4 且水量(this[19])非零 → sub_59C580 特殊地形（骨架）
    //   其余                              → sub_59A6C0 常规地形（第 1/2 段）
    wchar_t pathText[160];
    StringCchCopyW(pathText, ARRAYSIZE(pathText), L"未执行");
    if (ok)
    {
        if (cfg.landType == RmLandType::Inland || cfg.landType == RmLandType::Mountainous)
        {
            if (rmg.GetWaterAmount() != 0)
            {
                rmg.GenerateSpecialTerrain(); 
                // [SNAPSHOT-OFF] rmg.SaveStageSnapshot("GenerateSpecialTerrain");    // sub_59C580
                StringCchPrintfW(pathText, ARRAYSIZE(pathText),
                    L"sub_59C580 特殊地形（骨架，水量=%d）", rmg.GetWaterAmount());
            }
            else
            {
                StringCchCopyW(pathText, ARRAYSIZE(pathText),
                    L"跳过（水量=0，全陆地图）");
            }
        }
        else
        {
            rmg.GenerateTerrain();              // sub_59A6C0
            StringCchPrintfW(pathText, ARRAYSIZE(pathText),
                L"sub_59A6C0 常规地形（淹水 %d 格）", rmg.GetCellCount());
        }

        // sub_598960 @ 0x598b14: the water-detail pass runs for every land
        // type, unconditionally, right after the dispatch above.
        rmg.DecorateWaterTiles();               // sub_59C630
        // [SNAPSHOT-OFF] rmg.SaveStageSnapshot("DecorateWaterTiles");

        // 进度阶梯。vanilla 的 sub_643C50 tick 位置见 5994B0_Done.c；
        // 55% 与 60% 之间原版只夹了一段对话框泵，所以这里连续报两次。
        rmg.ReportProgress(55);                 // 0x598B4B
        rmg.ReportProgress(60);                 // 0x598BD1

        // sub_598960 @ 0x598C24: the Init-regions stage runs next, again for
        // every land type (0x598C24 - 0x598D42).
        rmg.InitRegions();                      // 0x598C24
        rmg.ReportProgress(65);                 // 0x598D34

        // sub_598960 @ 0x598D44: the Making-regions stage follows (0x598D44 -
        // 0x598E1E). Its cliff / region-merge / ramp chain is LandType 3/4 only;
        // the trailing placeholder fill runs for every land type.
        rmg.MakeRegions();                      // 0x598D44
        // 原版的 70% tick（0x598DBA）落在本阶段的悬崖链与绿地填充之间；端口把
        // 这两半合在 MakeRegions 里，所以 tick 放在其后。
        rmg.ReportProgress(70);                 // 0x598DBA

        // sub_598960 @ 0x598E1F: "Recalculating cell attributes" (0x598E1F -
        // 0x598E9E). Runs for every land type and is repeated three more times
        // further down the vanilla flow.
        rmg.RecalculateCellAttributes();        // 0x598E1F
        // [SNAPSHOT-OFF] rmg.SaveStageSnapshot("RecalculateCellAttributes_1");
        rmg.ReportProgress(75);                 // 0x598E99

        // sub_598960 @ 0x598E9E: "Creating starting points" (0x598E9E -
        // 0x598EBE). A retry loop over its two steps; the steps themselves are
        // still shells in this port.
        rmg.CreateStartingPoints();             // 0x598E9E

        // sub_598960 @ 0x598EBF: "Adding tech buildings" (0x598EBF - 0x598EE4).
        // Skipped only for LandType_Archipelago (0); every other land type,
        // including the Inland / Mountainous maps this port targets, runs it.
        rmg.AddTechBuildings();                 // 0x598EBF

        // sub_598960 @ 0x598EE5: "Creating tiberium" (0x598EE5 - 0x598FB7).
        // Grows the ore / gem fields, then tears the region system down.
        rmg.AddTiberium();                      // 0x598EE5

        // sub_598960 @ 0x598FB8: "Recalculating cell attributes" again -
        // the 2nd of the four passes (0x598FB8 - 0x599129). Same walk as the
        // first call; the UI / session tail that follows it is not ported.
        rmg.RecalculateCellAttributes();        // 0x598FB8
        // [SNAPSHOT-OFF] rmg.SaveStageSnapshot("RecalculateCellAttributes_2");
        rmg.ReportProgress(80);                 // 0x59902C
        rmg.ReportProgress(85);                 // 0x5990D1

        // sub_598960 @ 0x59912A: "Recalculating cell attributes" a third
        // time (0x59912A - 0x599170), right before the hills stage. Same walk;
        // only the psub_48D1D0 callback follows it, and that is not ported.
        rmg.RecalculateCellAttributes();        // 0x59912A
        // [SNAPSHOT-OFF] rmg.SaveStageSnapshot("RecalculateCellAttributes_3");

        // sub_598960 @ 0x599171: "Creating hills" (0x599171 - 0x599214).
        // The stage's six-step body is in place; every step is still a stub.
        rmg.CreateHills();                      // 0x599171
        // [SNAPSHOT-OFF] rmg.SaveStageSnapshot("CreateHills");
        rmg.ReportProgress(90);                 // 0x5991BC

        // sub_598960 @ 0x599215: "Creating LATs, rocks etc"
        // (0x599215 - 0x599353). The stage's four-step body is in place;
        // every step is still a stub.
        rmg.CreateLATs();                       // 0x599215
        // [SNAPSHOT-OFF] rmg.SaveStageSnapshot("CreateLATs");
        rmg.ReportProgress(95);                 // 0x5992EE

        // sub_598960 @ 0x599354: the fourth "Recalculating cell attributes"
        // pass (0x599354 - 0x5993A0). Same walk as the second and third calls;
        // the two engine-side calls that close the slice (sub_722D00 /
        // sub_722240, the tiberium growth + spread logic rebuild) are not ported.
        rmg.RecalculateCellAttributes();        // 0x599354
        // [SNAPSHOT-OFF] rmg.SaveStageSnapshot("RecalculateCellAttributes_4");

        // [移植侧] 瓦片全部定型后，剔除落在水面/悬崖上的树并让出生点周围
        // 少放树（必须在第 4 次重算之后，此时多格水/崖片已铺满）。
        rmg.PruneTerrainTrees();

        // sub_598960 @ 0x5993A5: the "RMG: Cleanup" teardown (0x5993A5 -
        // 0x59944C). A pure release; it changes nothing about the finished map.
        rmg.Cleanup();                          // 0x5993A5

        // sub_598960 @ 0x599451: the "RMG: Compute Radar Image" stage
        // (0x599451 - 0x599477). Builds the radar image (MapGenRadar.cpp) with
        // FinalAlert 2's own minimap algorithm and writes it to
        // radar_preview.png; the pixels are handed to the preview panel below.
        rmg.ComputeRadarImage();                // 0x599451

        // sub_598960 @ 0x59947E: the "RMG: Done" stage (0x59947E - 0x59951A),
        // the last stage of the main flow: the 100% progress tick, the
        // "RMG: Done" banner and the local progress bar teardown - see
        // MapGenDone.cpp for how each maps onto the port. It ends by detaching
        // the progress sink, so it stays the last call.
        rmg.Done();                             // 0x59947E

        // 把这一轮算出的雷达底图交给预览区。ComputeRadarImage 已经同时把它写成了
        // 程序目录下的 radar_preview.png。
        SetRadarBitmap(rmg.GetRadarImage());

        // ---- 落盘：把这一轮的结果写成 .map -------------------------------
        // 不是 vanilla 的步骤：原版由引擎自己的保存路径（MapClass 的
        // sub_4AD7E0）写盘，端口在此代劳。格式、11 字节格记录、IsoMapPack5 /
        // OverlayPack 的打包方式以及核对依据都在 MapGenMapFile.cpp 里。
        {
            // 输出目录用函数开头读到的 outDir（界面所选文件夹，默认 exe
            // 所在文件夹），它本身就是带反斜杠的绝对路径，直接拼文件名；
            // 目录不在就建。
            CreateDirectoryW(outDir, nullptr);      // 已存在时返回失败，忽略

            SYSTEMTIME now = {};
            GetLocalTime(&now);
            // 后缀按地图类型走：单人 .map，多人 .yrm（内容都是纯文本 INI，
            // 差别在 [Header] / [Basic] / [Houses]，由 SaveMapFile 按模式写）。
            StringCchPrintfW(mapPath, ARRAYSIZE(mapPath),
                             L"%srmg_%04d%02d%02d_%02d%02d%02d%s",
                             outDir, now.wYear, now.wMonth, now.wDay,
                             now.wHour, now.wMinute, now.wSecond,
                             p.gameType == 1 ? L".yrm" : L".map");

            if (!rmg.SaveMapFile(mapPath))
            {
                mapPath[0] = 0;
            }
            else
            {
                // 落盘成功：往输出目录下的 seed_history.txt 追加本轮种子
                const wchar_t* fileName = wcsrchr(mapPath, L'\\');
                fileName = fileName ? fileName + 1 : mapPath;
                AppendSeedHistory(outDir, now, LandTypeStr(cfg.landType), fileName,
                                  globalSeed, globalSeedInput != 0,
                                  mapSeedInput, mapSeedInput != 0);
            }
        }
    }

    // 生成结束：恢复按钮、摘掉本轮的面板指针（进度百分比由 UI 自己留着显示）
    if (generateButton)
        EnableWindow(generateButton, TRUE);
    g_progressWindow = nullptr;

    wchar_t text[1024];
    if (!ok)
    {
        StringCchPrintfW(text, ARRAYSIZE(text), L"地形生成失败！");
    }
    else
    {
        const MapSizeResult& sz = rmg.GetSize();
        const RMGSettings& st = rmg.GetSettings();

        // 当前剧场+时间对应的矿灯名（原版按时间索引 4 个建筑名）
        const std::vector<std::string>& lamps =
            p.theater ? st.SnowOrePatchLamps : st.TemperateOrePatchLamps;
        const char* lampName =
            (p.timeOfDay < (int)lamps.size()) ? lamps[p.timeOfDay].c_str() : "(无)";

        StringCchPrintfW(text, ARRAYSIZE(text),
            L"=== 地形主体生成成功 ===\n"
            L"地形：%s\n"
            L"剧场：%s\n"
            L"时间：%s\n"
            L"玩家数：%d\n"
            L"尺寸档位：%s\n"
            L"地图宽：%d 格\n"
            L"地图高：%d 格\n"
            L"工作数组边长：%d\n"
            L"工作单元数：%d\n"
            L"25%%随机标志：%s\n"
            L"地形路径：%s\n"
            L"随机种子：0x%04X\n"
            L"全局种子（%s）：%u (0x%08X)\n"
            L"地形种子（%s）：%u\n"
            L"\n--- RMGMD.INI 配置（sub_5981F0）---\n"
            L"配置来源：%s\n"
            L"矿量下限/上限：%d / %d\n"
            L"最大树木数：%d\n"
            L"光照等级（当前时间）：%d（%d/100）\n"
            L"环境光（当前剧场+时间）：%d\n"
            L"植被密度（当前地形）：%d - %d\n"
            L"矿区灯（当前剧场+时间）：%hs\n"
            L"雷达底图：radar_preview.png（输出目录）",
            LandTypeStr(cfg.landType),
            kTheaterOptions[p.theater],
            kTimeOptions[p.timeOfDay],
            cfg.playerCount,
            kSizeOptions[p.mapSize],
            sz.width, sz.height,
            sz.workSide,
            sz.workSide * sz.workSide,
            rmg.GetRandom25Flag() ? L"是" : L"否",
            pathText,
            cfg.randomSeed,
            globalSeedInput ? L"手填复现" : L"每轮随机",
            globalSeed, globalSeed,
            mapSeedInput ? L"手填换地形" : L"原版默认",
            mapSeedInput,
            rmg.GetIniLoaded() ? L"RMGMD.INI" : L"内置默认值",
            st.MinTiberium, st.MaxTiberium,
            st.MaxTrees,
            st.LevelLight(p.timeOfDay), st.LevelLight(p.timeOfDay),
            st.AmbientLight(p.theater, p.timeOfDay),
            st.VegetationMin((int)cfg.landType), st.VegetationMax((int)cfg.landType),
            lampName);

        // 地图文件路径（落盘成功时）
        if (mapPath[0])
        {
            StringCchCatW(text, ARRAYSIZE(text), L"\n地图文件：");
            StringCchCatW(text, ARRAYSIZE(text), mapPath);
        }
        else
        {
            StringCchCatW(text, ARRAYSIZE(text), L"\n地图文件：写入失败");
        }
    }

    MessageBoxW(hWnd, text, L"生成结果", MB_OK | MB_ICONINFORMATION);

    // 雷达底图换新了，重画右侧预览区
    InvalidateRect(hWnd, nullptr, TRUE);
}

// 窗口过程（处理窗口消息，比如按钮点击）
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;

        // 统一字体：默认系统字体的中文渲染发虚，换成微软雅黑
        g_uiFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        // 逐行创建“标签 + 下拉框”参数UI
        for (int i = 0; i < ARRAYSIZE(kParamRows); ++i)
        {
            const ParamRow& row = kParamRows[i];
            int y = 16 + i * 32;

            CreateCtl(hWnd, L"Static", row.label,
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                28, y, 84, 26, 0, cs->hInstance);

            HWND combo = CreateCtl(hWnd, L"ComboBox", nullptr,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                124, y, 240, 200, row.comboId, cs->hInstance);
            // 注意：ComboBox的高度参数包含下拉展开后的列表高度，200保证7项全部可见

            for (int j = 0; j < row.optionCount; ++j)
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)row.options[j]);
            SendMessageW(combo, CB_SETCURSEL, 0, 0); // 默认选中第一项
        }

        // [已移除] 两个种子输入框（全局种子 / 地形种子）。
        // 界面上一并删掉，这里只把原来的创建代码整段注释保留，不做删除。
        // 控件不存在时 SeedEditValue 读不到值，按 0 处理，于是行为退回默认：
        // 全局种子 0 = 每轮随机（GetTickCount），地形种子 0 = 原版固定种子 0。
        //
        //// 两个种子输入框。默认文本 "0"：全局种子 0 = 每轮随机，地形种子 0 =
        //// 原版固定 0；填具体数字就按填的种子生成，相同数字出相同地图。
        //static const struct SeedRow { const wchar_t* label; int id; const wchar_t* hint; }
        //    kSeedRows[] =
        //{
        //    { L"全局种子", ID_GLOBALSEED_EDIT, L"0=每次随机，填数=复现" },
        //    { L"地形种子", ID_MAPSEED_EDIT,    L"0=原版默认，填数=换地形" },
        //};
        //for (int i = 0; i < ARRAYSIZE(kSeedRows); ++i)
        //{
        //    const int y = 240 + i * 30;
        //    CreateCtl(hWnd, L"Static", kSeedRows[i].label,
        //        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        //        24, y, 72, 24, 0, cs->hInstance);
        //    CreateCtl(hWnd, L"Edit", L"0",
        //        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP
        //        | ES_AUTOHSCROLL | ES_NUMBER,
        //        100, y, 150, 24, kSeedRows[i].id, cs->hInstance);
        //    CreateCtl(hWnd, L"Static", kSeedRows[i].hint,
        //        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        //        256, y, 130, 24, 0, cs->hInstance);
        //}

        // 输出目录行（占用原种子框空出的 y=240 位置）。
        // 路径框默认显示 exe 所在文件夹：产物默认就落在 exe 旁边；
        // 点"浏览…"可让使用者自选任意文件夹。路径框只读，只能通过对话框改，
        // 避免手敲出非法路径。
        {
            wchar_t exeDir[MAX_PATH] = {};
            GetExeDir(exeDir, ARRAYSIZE(exeDir));

            CreateCtl(hWnd, L"Static", L"输出目录",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                28, 240, 84, 26, 0, cs->hInstance);
            CreateCtl(hWnd, L"Edit", exeDir,
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
                124, 242, 176, 24, ID_OUTDIR_EDIT, cs->hInstance);
            CreateCtl(hWnd, L"Button", L"浏览…",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                308, 240, 56, 26, ID_BROWSE_BTN, cs->hInstance);
        }

        CreateCtl(hWnd, L"Button", L"生成",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            152, 302, 120, 36, ID_GENERATE_BTN, cs->hInstance);

        // 清除输出：删掉所选输出文件夹下历次生成的 .map 和阶段快照
        CreateCtl(hWnd, L"Button", L"清除输出",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            280, 302, 84, 36, ID_CLEAR_BTN, cs->hInstance);

        // 右侧预览面板里的进度条；位置由 LayoutPreviewPanel 按面板矩形摆好。
        g_progressBar = CreateWindowExW(0, PROGRESS_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            0, 0, 10, kBarHeight, hWnd, (HMENU)(INT_PTR)ID_PROGRESS_BAR,
            cs->hInstance, nullptr);
        if (g_progressBar)
        {
            if (g_uiFont)
                SendMessageW(g_progressBar, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
            SendMessageW(g_progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            SendMessageW(g_progressBar, PBM_SETPOS, 0, 0);
        }
        LayoutPreviewPanel(hWnd);
        break;
    }
    case WM_SIZE:
        // 只有预览面板里的进度条需要跟着窗口尺寸走，左侧控件位置固定
        LayoutPreviewPanel(hWnd);
        break;
    case WM_CTLCOLORSTATIC:
        // 静态标签默认背景是灰色按钮底色，这里改成与窗口一致的白色
        SetBkColor((HDC)wParam, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        // 右侧预览面板：留出左边距、上边距和四周内缩
        const RECT panel = PreviewPanelRect(hWnd);
        if (panel.right > panel.left && panel.bottom > panel.top)
        {
            // 面板标题 + 进度行（进度条本身是子控件，见 WM_CREATE）
            HGDIOBJ oldFont = g_uiFont ? SelectObject(hdc, g_uiFont) : nullptr;
            SetBkMode(hdc, TRANSPARENT);
            RECT title = { panel.left, panel.top,
                           panel.right, panel.top + kTitleHeight };
            DrawTextW(hdc, L"雷达底图预览", -1, &title,
                      DT_CENTER | DT_SINGLELINE | DT_VCENTER);

            wchar_t progressText[64];
            if (g_progressPercent >= 0)
            {
                StringCchPrintfW(progressText, ARRAYSIZE(progressText),
                                 L"生成进度：%d%%", g_progressPercent);
            }
            else
            {
                StringCchCopyW(progressText, ARRAYSIZE(progressText), L"尚未生成");
            }
            RECT status = { panel.left, panel.top + kTitleHeight,
                            panel.right,
                            panel.top + kTitleHeight + kStatusHeight };
            // 进度刷新是不擦背景的局部重绘（InvalidateRect 传了 FALSE），文字
            // 背景又是透明模式；不先刷白，新百分比会直接叠在旧数字上糊成一团。
            FillRect(hdc, &status, (HBRUSH)(COLOR_WINDOW + 1));
            DrawTextW(hdc, progressText, -1, &status,
                      DT_CENTER | DT_SINGLELINE | DT_VCENTER);

            // 图像区域：两行文字 + 进度条下方，四角内缩；外面套一圈边框
            RECT view = { panel.left + 2, panel.top + kViewTop,
                          panel.right - 2, panel.bottom - 2 };
            FrameRect(hdc, &view, (HBRUSH)GetStockObject(GRAY_BRUSH));

            if (g_radarBitmap && g_radarWidth > 0 && g_radarHeight > 0)
            {
                // 等比缩放并居中
                const int vw = view.right - view.left;
                const int vh = view.bottom - view.top;
                double scale = static_cast<double>(vw) / g_radarWidth;
                const double scaleY = static_cast<double>(vh) / g_radarHeight;
                if (scaleY < scale)
                    scale = scaleY;

                int w = static_cast<int>(g_radarWidth * scale + 0.5);
                int h = static_cast<int>(g_radarHeight * scale + 0.5);
                if (w < 1) w = 1;
                if (h < 1) h = 1;
                const int x = view.left + (vw - w) / 2;
                const int y = view.top + (vh - h) / 2;

                HDC mem = CreateCompatibleDC(hdc);
                HGDIOBJ old = SelectObject(mem, g_radarBitmap);
                // COLORONCOLOR = 直接取样，放大后仍是硬边像素，不会糊
                SetStretchBltMode(hdc, COLORONCOLOR);
                StretchBlt(hdc, x, y, w, h, mem, 0, 0,
                           g_radarWidth, g_radarHeight, SRCCOPY);
                SelectObject(mem, old);
                DeleteDC(mem);
            }
            else
            {
                RECT hint = view;
                DrawTextW(hdc, L"点“生成”后，这里显示雷达底图", -1, &hint,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            if (oldFont)
                SelectObject(hdc, oldFont);
        }

        EndPaint(hWnd, &ps);
        break;
    }
    case WM_COMMAND:
    {
        // 处理按钮点击事件
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == ID_GENERATE_BTN)
        {
            OnGenerate(hWnd);
        }
        else if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == ID_CLEAR_BTN)
        {
            OnClearOutputs(hWnd);
        }
        else if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == ID_BROWSE_BTN)
        {
            OnBrowseOutputDir(hWnd);
        }
        break;
    }
    case WM_DESTROY:
        // 子控件随父窗口一起销毁，这里只清指针
        g_progressBar = nullptr;
        if (g_radarBitmap)
        {
            DeleteObject(g_radarBitmap);
            g_radarBitmap = nullptr;
        }
        if (g_uiFont)
        {
            DeleteObject(g_uiFont);
            g_uiFont = nullptr;
        }
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int __stdcall wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    // [FA2sp] The standalone tool keeps the original behaviour: read RMGMD.INI
    // from its own folder. The editor extension leaves both defaults alone.
    RandomMapGenerator::s_bLoadRmgmdIni = true;

    // 进度条控件在 comctl32 里，用之前先初始化
    INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    // 新版"选择文件夹"对话框（BIF_NEWDIALOGSTYLE）要求先初始化 COM。
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 1. 注册窗口类
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;          // 绑定窗口过程
    wc.hInstance = hInstance;          // 当前实例
    wc.lpszClassName = L"MapGeneratorWindowClass"; // 窗口类名
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc)) return 1;

    // 2. 创建主窗口
    HWND hWnd = CreateWindowW(
        L"MapGeneratorWindowClass",    // 类名
        L"地图生成器",                  // 窗口标题
        WS_OVERLAPPEDWINDOW,           // 窗口样式
        CW_USEDEFAULT, CW_USEDEFAULT,  // 默认位置
        880, 400,                      // 窗口大小（左侧参数控件 + 右侧雷达预览）
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) return 1;

    // 3. 显示窗口并更新
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    // 4. 消息循环
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();   // 和开头的 CoInitializeEx 配对
    return (int)msg.wParam;
}
