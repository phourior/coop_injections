#pragma once

#include <QtWidgets/QMainWindow>
#include <QTimer>
#include <QSet>
#include "ui_ZeratulRelicScanner.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <string>

class ZeratulRelicScanner : public QMainWindow
{
    Q_OBJECT

public:
    ZeratulRelicScanner(QWidget *parent = nullptr);
    ~ZeratulRelicScanner();

private slots:
    void onStartMonitor();
    void onStopMonitor();
    void onPollTick();

private:
    void setupStyle();
    void appendLog(const QString& msg);
    DWORD findProcess(const wchar_t* exeName);
    bool injectDll(DWORD pid, const std::wstring& dllPath);
    void updateStatusUi(bool running, DWORD pid);
    void updateInjectUi(bool injected);
    // 从 QRC 资源解压 DLL 到临时目录，返回绝对路径（失败返回空串）
    QString extractDllFromResource();

    Ui::ZeratulRelicScannerClass ui;
    QTimer* m_pollTimer = nullptr;

    // 已注入过的 PID 集合（进程重启会产生新 PID，自动重新注入）
    QSet<DWORD> m_injectedPids;

    // 上次检测到的 PID（用于检测进程退出）
    DWORD m_lastPid = 0;

    // 解压后的 DLL 临时路径（空串表示尚未解压）
    QString m_extractedDllPath;
};

