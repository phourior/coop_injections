#pragma once

#include <QtWidgets/QMainWindow>
#include <QTimer>
#include "ui_ZeratulRelicScanner.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <vector>
#include <string>

struct ProcessInfo {
    DWORD pid;
    QString name;
};

class ZeratulRelicScanner : public QMainWindow
{
    Q_OBJECT

public:
    ZeratulRelicScanner(QWidget *parent = nullptr);
    ~ZeratulRelicScanner();

private slots:
    void onRefreshProcess();
    void onAddDll();
    void onRemoveDll();
    void onClearDll();
    void onInject();

private:
    void setupStyle();
    void loadDefaults();
    std::vector<ProcessInfo> enumerateProcesses();
    bool injectDll(DWORD pid, const std::string& dllPath);

    Ui::ZeratulRelicScannerClass ui;
    QString m_defaultProcessName;
    QString m_defaultDllPath;
};

