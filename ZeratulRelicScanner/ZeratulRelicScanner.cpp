#include "ZeratulRelicScanner.h"
#include <QDateTime>
#include <QDir>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFile>
#include <QStandardPaths>
#include <QStyle>

// ─── 轮询间隔（毫秒） ────────────────────────────────────────────────────────
static constexpr int kPollIntervalMs = 2000;
static constexpr const wchar_t* kTargetExe  = L"SC2_x64.exe";
static constexpr const char*    kRcDllPath  = ":/ZeratulRelicScanner/dll1.dll";
static constexpr const char*    kDllName    = "dll1.dll";

// ─── 构造 ─────────────────────────────────────────────────────────────────────
ZeratulRelicScanner::ZeratulRelicScanner(QWidget *parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);
    setupStyle();

    // 初始 UI 状态
    updateStatusUi(false, 0);
    updateInjectUi(false);

    // 尝试从资源解压 DLL
    m_extractedDllPath = extractDllFromResource();
    if (m_extractedDllPath.isEmpty()) {
        ui.lblDllPath->setText("[错误] 资源中未找到 dll1.dll");
        appendLog("[错误] 无法从资源中解压 dll1.dll，请检查 QRC 文件。");
    } else {
        ui.lblDllPath->setText(m_extractedDllPath);
        appendLog(QString("DLL 已解压至: %1").arg(m_extractedDllPath));
    }

    // 定时器
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &ZeratulRelicScanner::onPollTick);

    // 按钮信号
    connect(ui.btnStart, &QPushButton::clicked, this, &ZeratulRelicScanner::onStartMonitor);
    connect(ui.btnStop,  &QPushButton::clicked, this, &ZeratulRelicScanner::onStopMonitor);

    appendLog("程序启动，等待开始监控...");
}

// ─── 从 QRC 解压 DLL ──────────────────────────────────────────────────────────
QString ZeratulRelicScanner::extractDllFromResource()
{
    QFile src(kRcDllPath);
    if (!src.exists())
        return {};

    // 解压到 %TEMP%\sc2loader\dll1.dll
    QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                     + "/sc2loader";
    QDir().mkpath(tmpDir);
    QString outPath = tmpDir + "/" + kDllName;

    // 每次都覆盖写入（确保资源与磁盘同步）
    if (QFile::exists(outPath))
        QFile::remove(outPath);

    if (!src.open(QIODevice::ReadOnly))
        return {};

    QFile dst(outPath);
    if (!dst.open(QIODevice::WriteOnly))
        return {};

    dst.write(src.readAll());
    dst.close();
    src.close();
    return outPath;
}

ZeratulRelicScanner::~ZeratulRelicScanner()
{
}

// ─── 样式 ─────────────────────────────────────────────────────────────────────
void ZeratulRelicScanner::setupStyle()
{
    setStyleSheet(R"(
        QMainWindow { background-color: #1e1e2e; }
        QWidget { background-color: #1e1e2e; color: #cdd6f4; }
        QGroupBox {
            font-weight: bold;
            font-size: 13px;
            border: 1px solid #45475a;
            border-radius: 6px;
            margin-top: 10px;
            padding-top: 18px;
            color: #cdd6f4;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 6px;
            color: #89b4fa;
        }
        QLabel { color: #cdd6f4; font-size: 12px; }
        QLabel#lblProcessStatus[running="true"]  { color: #a6e3a1; font-weight: bold; }
        QLabel#lblProcessStatus[running="false"] { color: #f38ba8; font-weight: bold; }
        QLabel#lblInjectStatus[injected="true"]  { color: #a6e3a1; font-weight: bold; }
        QLabel#lblInjectStatus[injected="false"] { color: #fab387; font-weight: bold; }
        QTextEdit {
            background-color: #181825;
            color: #a6e3a1;
            border: 1px solid #45475a;
            border-radius: 4px;
            font-family: Consolas, monospace;
            font-size: 11px;
        }
        QPushButton {
            border: none;
            border-radius: 6px;
            padding: 8px 24px;
            font-size: 13px;
            font-weight: bold;
        }
        QPushButton#btnStart {
            background-color: #89b4fa;
            color: #1e1e2e;
        }
        QPushButton#btnStart:hover  { background-color: #74c7ec; }
        QPushButton#btnStart:disabled { background-color: #45475a; color: #6c7086; }
        QPushButton#btnStop {
            background-color: #f38ba8;
            color: #1e1e2e;
        }
        QPushButton#btnStop:hover   { background-color: #eba0ac; }
        QPushButton#btnStop:disabled { background-color: #45475a; color: #6c7086; }
        QStatusBar { background-color: #181825; color: #6c7086; font-size: 11px; }
    )");
}

// ─── 日志辅助 ─────────────────────────────────────────────────────────────────
void ZeratulRelicScanner::appendLog(const QString& msg)
{
    QString ts = QDateTime::currentDateTime().toString("hh:mm:ss");
    ui.logView->append(QString("[%1] %2").arg(ts, msg));
}

// ─── UI 更新辅助 ──────────────────────────────────────────────────────────────
void ZeratulRelicScanner::updateStatusUi(bool running, DWORD pid)
{
    if (running) {
        ui.lblProcessStatus->setText("运行中");
        ui.lblProcessStatus->setProperty("running", true);
        ui.lblPid->setText(QString::number(pid));
    } else {
        ui.lblProcessStatus->setText("未运行");
        ui.lblProcessStatus->setProperty("running", false);
        ui.lblPid->setText("—");
    }
    // 刷新样式
    ui.lblProcessStatus->style()->unpolish(ui.lblProcessStatus);
    ui.lblProcessStatus->style()->polish(ui.lblProcessStatus);
}

void ZeratulRelicScanner::updateInjectUi(bool injected)
{
    if (injected) {
        ui.lblInjectStatus->setText("已注入");
        ui.lblInjectStatus->setProperty("injected", true);
    } else {
        ui.lblInjectStatus->setText("未注入");
        ui.lblInjectStatus->setProperty("injected", false);
    }
    ui.lblInjectStatus->style()->unpolish(ui.lblInjectStatus);
    ui.lblInjectStatus->style()->polish(ui.lblInjectStatus);
}

// ─── 查找进程 ─────────────────────────────────────────────────────────────────
DWORD ZeratulRelicScanner::findProcess(const wchar_t* exeName)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);

    DWORD found = 0;
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                found = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }

    CloseHandle(hSnap);
    return found;
}

// ─── DLL 注入（LoadLibraryW 远程线程） ────────────────────────────────────────
bool ZeratulRelicScanner::injectDll(DWORD pid, const std::wstring& dllPath)
{
    HANDLE hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                  FALSE, pid);
    if (!hProcess)
        return false;

    SIZE_T byteSize = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remoteMem = VirtualAllocEx(hProcess, nullptr, byteSize,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        CloseHandle(hProcess);
        return false;
    }

    if (!WriteProcessMemory(hProcess, remoteMem, dllPath.c_str(), byteSize, nullptr)) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    FARPROC pLoadLibraryW = GetProcAddress(hKernel32, "LoadLibraryW");
    if (!pLoadLibraryW) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(pLoadLibraryW),
        remoteMem, 0, nullptr);

    if (!hThread) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    WaitForSingleObject(hThread, 8000);

    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProcess);
    return true;
}

// ─── 开始监控 ────────────────────────────────────────────────────────────────
void ZeratulRelicScanner::onStartMonitor()
{
    m_injectedPids.clear();
    m_lastPid = 0;
    m_pollTimer->start();

    ui.btnStart->setEnabled(false);
    ui.btnStop->setEnabled(true);
    statusBar()->showMessage("监控中...");
    appendLog("开始监控 SC2_x64.exe ...");

    // 立即执行一次检测
    onPollTick();
}

// ─── 停止监控 ────────────────────────────────────────────────────────────────
void ZeratulRelicScanner::onStopMonitor()
{
    m_pollTimer->stop();

    ui.btnStart->setEnabled(true);
    ui.btnStop->setEnabled(false);
    statusBar()->showMessage("监控已停止");
    appendLog("监控已停止。");

    updateStatusUi(false, 0);
    updateInjectUi(false);
    m_lastPid = 0;
}

// ─── 轮询检测 ────────────────────────────────────────────────────────────────
void ZeratulRelicScanner::onPollTick()
{
    DWORD pid = findProcess(kTargetExe);

    if (pid == 0) {
        // 进程不在运行
        if (m_lastPid != 0) {
            appendLog("SC2_x64.exe 已退出。");
            updateStatusUi(false, 0);
            updateInjectUi(false);
            m_lastPid = 0;
        }
        return;
    }

    // 进程在运行
    if (pid != m_lastPid) {
        m_lastPid = pid;
        appendLog(QString("检测到 SC2_x64.exe  PID=%1").arg(pid));
        updateStatusUi(true, pid);
    }

    // 尚未注入此 PID
    if (!m_injectedPids.contains(pid)) {
        if (m_extractedDllPath.isEmpty()) {
            appendLog("[错误] DLL 尚未解压，无法注入。");
            statusBar()->showMessage("DLL 解压失败！", 5000);
            return;
        }

        QString dllQPath = m_extractedDllPath;
        appendLog(QString("正在注入 %1 → PID %2 ...").arg(kDllName).arg(pid));

        std::wstring dllWPath = dllQPath.toStdWString();
        if (injectDll(pid, dllWPath)) {
            m_injectedPids.insert(pid);
            appendLog("注入成功！");
            updateInjectUi(true);
            statusBar()->showMessage(QString("已成功注入 PID %1").arg(pid), 5000);
        } else {
            DWORD err = GetLastError();
            appendLog(QString("[错误] 注入失败，错误码: %1  请以管理员权限运行。").arg(err));
            statusBar()->showMessage("注入失败，请以管理员权限运行", 5000);
        }
    }
}



