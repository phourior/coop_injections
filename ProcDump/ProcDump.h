#pragma once

#include <QtWidgets/QMainWindow>
#include "ui_ProcDump.h"

class QCheckBox;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTextEdit;

class ProcDump : public QMainWindow
{
    Q_OBJECT

public:
    ProcDump(QWidget *parent = nullptr);
    ~ProcDump();

private:
    void buildUi();
    void refreshProcesses();
    void chooseDumpDirectory();
    void dumpSelectedProcess();
    void appendLog(const QString& message);
    unsigned long selectedPid() const;

    Ui::ProcDumpClass ui;
    QLineEdit* filterEdit = nullptr;
    QLineEdit* pidEdit = nullptr;
    QLineEdit* dumpDirEdit = nullptr;
    QTableWidget* processTable = nullptr;
    QTextEdit* logEdit = nullptr;
    QCheckBox* rwOnlyCheck = nullptr;
    QCheckBox* allReadableCheck = nullptr;
    QCheckBox* includeImageCheck = nullptr;
    QCheckBox* includeMappedCheck = nullptr;
    QCheckBox* includePrivateCheck = nullptr;
    QPushButton* dumpButton = nullptr;
};

