#include "ZeratulRelicScanner.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QHeaderView>
#include <algorithm>

ZeratulRelicScanner::ZeratulRelicScanner(QWidget *parent)
    : QMainWindow(parent)
    , m_defaultProcessName("SC2_x64.exe")
    , m_defaultDllPath("dll1.dll")
{
    ui.setupUi(this);
    setupStyle();

    // 表格列宽设置
    ui.tableDll->horizontalHeader()->setStretchLastSection(true);
    ui.tableDll->setColumnWidth(0, 250);

    // 信号连接
    connect(ui.btnRefreshProcess, &QPushButton::clicked, this, &ZeratulRelicScanner::onRefreshProcess);
    connect(ui.btnAddDll, &QPushButton::clicked, this, &ZeratulRelicScanner::onAddDll);
    connect(ui.btnRemoveDll, &QPushButton::clicked, this, &ZeratulRelicScanner::onRemoveDll);
    connect(ui.btnClearDll, &QPushButton::clicked, this, &ZeratulRelicScanner::onClearDll);
    connect(ui.btnInject, &QPushButton::clicked, this, &ZeratulRelicScanner::onInject);

    loadDefaults();
}

ZeratulRelicScanner::~ZeratulRelicScanner()
{
}

void ZeratulRelicScanner::setupStyle()
{
    setStyleSheet(R"(
        QMainWindow { background-color: #f0f0f0; }
        QGroupBox {
            font-weight: bold;
            border: 1px solid #cccccc;
            border-radius: 4px;
            margin-top: 8px;
            padding-top: 16px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 5px;
            color: #c0392b;
        }
        QPushButton#btnRefreshProcess {
            background-color: #3498db;
            color: white;
            border: none;
            border-radius: 4px;
            padding: 6px 16px;
        }
        QPushButton#btnRefreshProcess:hover { background-color: #2980b9; }
        QPushButton#btnAddDll {
            background-color: #3498db;
            color: white;
            border: none;
            border-radius: 4px;
            padding: 6px 16px;
        }
        QPushButton#btnAddDll:hover { background-color: #2980b9; }
        QPushButton#btnRemoveDll {
            background-color: #e67e22;
            color: white;
            border: none;
            border-radius: 4px;
            padding: 6px 16px;
        }
        QPushButton#btnRemoveDll:hover { background-color: #d35400; }
        QPushButton#btnClearDll {
            background-color: #e74c3c;
            color: white;
            border: none;
            border-radius: 4px;
            padding: 6px 16px;
        }
        QPushButton#btnClearDll:hover { background-color: #c0392b; }
        QPushButton#btnInject {
            background-color: #e74c3c;
            color: white;
            border: none;
            border-radius: 6px;
            padding: 8px 32px;
            font-size: 14px;
            font-weight: bold;
        }
        QPushButton#btnInject:hover { background-color: #c0392b; }
        QTableWidget {
            background-color: white;
            color: #333333;
            gridline-color: #e0e0e0;
            selection-background-color: #3498db;
            selection-color: white;
        }
        QHeaderView::section {
            background-color: #f5f5f5;
            color: #333333;
            padding: 4px;
            border: 1px solid #e0e0e0;
            font-weight: bold;
        }
        QComboBox {
            padding: 4px 8px;
            border: 1px solid #bdc3c7;
            border-radius: 3px;
        }
    )");
}

void ZeratulRelicScanner::loadDefaults()
{
    // 刷新进程列表
    onRefreshProcess();

    // 尝试选中默认进程
    for (int i = 0; i < ui.comboProcess->count(); ++i) {
        if (ui.comboProcess->itemText(i).contains(m_defaultProcessName, Qt::CaseInsensitive)) {
            ui.comboProcess->setCurrentIndex(i);
            break;
        }
    }

    // 添加默认DLL（本目录下的dll1.dll）
    QString appDir = QCoreApplication::applicationDirPath();
    QString dllFullPath = QDir(appDir).absoluteFilePath(m_defaultDllPath);
    QFileInfo fi(dllFullPath);

    int row = ui.tableDll->rowCount();
    ui.tableDll->insertRow(row);
    ui.tableDll->setItem(row, 0, new QTableWidgetItem(fi.fileName()));
    ui.tableDll->setItem(row, 1, new QTableWidgetItem(dllFullPath));

    statusBar()->showMessage(QString("默认DLL: %1 | 默认进程: %2").arg(dllFullPath, m_defaultProcessName));
}

std::vector<ProcessInfo> ZeratulRelicScanner::enumerateProcesses()
{
    std::vector<ProcessInfo> procs;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE)
        return procs;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(hSnap, &pe)) {
        do {
            ProcessInfo info;
            info.pid = pe.th32ProcessID;
            info.name = QString::fromWCharArray(pe.szExeFile);
            if (info.pid != 0) {
                procs.push_back(info);
            }
        } while (Process32NextW(hSnap, &pe));
    }

    CloseHandle(hSnap);

    // 按进程名排序
    std::sort(procs.begin(), procs.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
        return a.name.toLower() < b.name.toLower();
    });

    return procs;
}

void ZeratulRelicScanner::onRefreshProcess()
{
    ui.comboProcess->clear();
    auto procs = enumerateProcesses();
    for (const auto& p : procs) {
        ui.comboProcess->addItem(
            QString("%1 (PID: %2)").arg(p.name).arg(p.pid),
            QVariant(static_cast<uint>(p.pid))
        );
    }
    statusBar()->showMessage(QString("已刷新进程列表，共 %1 个进程").arg(procs.size()), 3000);
}

void ZeratulRelicScanner::onAddDll()
{
    QStringList files = QFileDialog::getOpenFileNames(
        this, "选择DLL文件",
        QCoreApplication::applicationDirPath(),
        "DLL Files (*.dll);;All Files (*.*)"
    );

    for (const QString& file : files) {
        QFileInfo fi(file);
        int row = ui.tableDll->rowCount();
        ui.tableDll->insertRow(row);
        ui.tableDll->setItem(row, 0, new QTableWidgetItem(fi.fileName()));
        ui.tableDll->setItem(row, 1, new QTableWidgetItem(fi.absoluteFilePath()));
    }
}

void ZeratulRelicScanner::onRemoveDll()
{
    QList<int> rows;
    for (auto item : ui.tableDll->selectedItems()) {
        if (!rows.contains(item->row()))
            rows.append(item->row());
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows) {
        ui.tableDll->removeRow(row);
    }
}

void ZeratulRelicScanner::onClearDll()
{
    ui.tableDll->setRowCount(0);
}

bool ZeratulRelicScanner::injectDll(DWORD pid, const std::string& dllPath)
{
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess) {
        return false;
    }

    SIZE_T pathSize = dllPath.size() + 1;
    LPVOID remoteMem = VirtualAllocEx(hProcess, nullptr, pathSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        CloseHandle(hProcess);
        return false;
    }

    if (!WriteProcessMemory(hProcess, remoteMem, dllPath.c_str(), pathSize, nullptr)) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    FARPROC pLoadLibraryA = GetProcAddress(hKernel32, "LoadLibraryA");
    if (!pLoadLibraryA) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(pLoadLibraryA),
        remoteMem, 0, nullptr);

    if (!hThread) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    WaitForSingleObject(hThread, 5000);

    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProcess);
    return true;
}

void ZeratulRelicScanner::onInject()
{
    if (ui.comboProcess->currentIndex() < 0) {
        QMessageBox::warning(this, "警告", "请先选择一个目标进程！");
        return;
    }

    if (ui.tableDll->rowCount() == 0) {
        QMessageBox::warning(this, "警告", "请先添加DLL文件！");
        return;
    }

    DWORD pid = ui.comboProcess->currentData().toUInt();
    if (pid == 0) {
        QMessageBox::warning(this, "警告", "无效的进程ID！");
        return;
    }

    int successCount = 0;
    int failCount = 0;

    for (int i = 0; i < ui.tableDll->rowCount(); ++i) {
        QString dllPath = ui.tableDll->item(i, 1)->text();
        QFileInfo fi(dllPath);
        if (!fi.exists()) {
            QMessageBox::warning(this, "警告",
                QString("DLL文件不存在: %1").arg(dllPath));
            failCount++;
            continue;
        }

        std::string pathStr = dllPath.toStdString();
        if (injectDll(pid, pathStr)) {
            successCount++;
        } else {
            failCount++;
        }
    }

    if (failCount == 0) {
        QMessageBox::information(this, "成功",
            QString("成功注入 %1 个DLL！").arg(successCount));
    } else {
        QMessageBox::warning(this, "结果",
            QString("成功: %1, 失败: %2\n请确保以管理员权限运行。").arg(successCount).arg(failCount));
    }

    statusBar()->showMessage(QString("注入完成 - 成功: %1, 失败: %2").arg(successCount).arg(failCount), 5000);
}

