#include "ProcDump.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace {
struct ProcessRow {
    DWORD pid = 0;
    QString name;
    QString path;
};

struct DumpOptions {
    DWORD pid = 0;
    QString outputDir;
    bool rwOnly = true;
    bool allReadable = false;
    bool includeImage = true;
    bool includeMapped = true;
    bool includePrivate = true;
};

struct DumpStats {
    int regions = 0;
    int modules = 0;
    quint64 bytesWritten = 0;
};

QString winErrorText(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD len = FormatMessageW(flags, nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    QString text = len && buffer ? QString::fromWCharArray(buffer).trimmed() : QStringLiteral("error %1").arg(error);
    if (buffer) {
        LocalFree(buffer);
    }
    return text;
}

std::string narrow(const QString& value) {
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString protectName(DWORD protect) {
    QStringList flags;
    if (protect & PAGE_GUARD) {
        flags << "GUARD";
    }
    if (protect & PAGE_NOCACHE) {
        flags << "NOCACHE";
    }
    if (protect & PAGE_WRITECOMBINE) {
        flags << "WRITECOMBINE";
    }

    switch (protect & 0xff) {
    case PAGE_EXECUTE: flags.prepend("EXECUTE"); break;
    case PAGE_EXECUTE_READ: flags.prepend("EXECUTE_READ"); break;
    case PAGE_EXECUTE_READWRITE: flags.prepend("EXECUTE_READWRITE"); break;
    case PAGE_EXECUTE_WRITECOPY: flags.prepend("EXECUTE_WRITECOPY"); break;
    case PAGE_NOACCESS: flags.prepend("NOACCESS"); break;
    case PAGE_READONLY: flags.prepend("READONLY"); break;
    case PAGE_READWRITE: flags.prepend("READWRITE"); break;
    case PAGE_WRITECOPY: flags.prepend("WRITECOPY"); break;
    default: flags.prepend(QStringLiteral("0x%1").arg(protect & 0xff, 0, 16)); break;
    }
    return flags.join('|');
}

QString typeName(DWORD type) {
    switch (type) {
    case MEM_IMAGE: return QStringLiteral("IMAGE");
    case MEM_MAPPED: return QStringLiteral("MAPPED");
    case MEM_PRIVATE: return QStringLiteral("PRIVATE");
    default: return QStringLiteral("0x%1").arg(type, 0, 16);
    }
}

bool isReadable(DWORD protect) {
    if ((protect & PAGE_GUARD) || (protect & PAGE_NOACCESS)) {
        return false;
    }
    switch (protect & 0xff) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool isWritable(DWORD protect) {
    if ((protect & PAGE_GUARD) || (protect & PAGE_NOACCESS)) {
        return false;
    }
    switch (protect & 0xff) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool typeAllowed(DWORD type, const DumpOptions& options) {
    if (type == MEM_IMAGE) {
        return options.includeImage;
    }
    if (type == MEM_MAPPED) {
        return options.includeMapped;
    }
    if (type == MEM_PRIVATE) {
        return options.includePrivate;
    }
    return false;
}

std::string hex64(quint64 value, int width = 0) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    if (width > 0) {
        out << std::setw(width);
    }
    out << value;
    return out.str();
}

bool enableDebugPrivilege(QString* errorText) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        if (errorText) {
            *errorText = winErrorText(GetLastError());
        }
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        if (errorText) {
            *errorText = winErrorText(GetLastError());
        }
        CloseHandle(token);
        return false;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(token);

    if (err != ERROR_SUCCESS) {
        if (errorText) {
            *errorText = winErrorText(err);
        }
        return false;
    }
    return true;
}

QString queryProcessPath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return {};
    }

    std::vector<wchar_t> buffer(32768);
    DWORD size = static_cast<DWORD>(buffer.size());
    QString path;
    if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) {
        path = QString::fromWCharArray(buffer.data(), static_cast<int>(size));
    }
    CloseHandle(process);
    return path;
}

std::vector<ProcessRow> enumerateProcesses() {
    std::vector<ProcessRow> rows;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return rows;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            ProcessRow row;
            row.pid = entry.th32ProcessID;
            row.name = QString::fromWCharArray(entry.szExeFile);
            row.path = queryProcessPath(row.pid);
            rows.push_back(row);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    std::sort(rows.begin(), rows.end(), [](const ProcessRow& a, const ProcessRow& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return rows;
}

int writeModuleList(DWORD pid, const QString& dirPath) {
    const QString output = QDir(dirPath).filePath(QStringLiteral("modules.tsv"));
    std::ofstream out(narrow(output), std::ios::binary);
    out << "base\tsize\tmodule\tpath\n";

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        out << "ERROR\t0\tCreateToolhelp32Snapshot\t" << narrow(winErrorText(GetLastError())) << "\n";
        return 0;
    }

    int count = 0;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            out << "0x" << hex64(reinterpret_cast<quint64>(entry.modBaseAddr), 16)
                << "\t0x" << hex64(entry.modBaseSize)
                << "\t" << narrow(QString::fromWCharArray(entry.szModule))
                << "\t" << narrow(QString::fromWCharArray(entry.szExePath))
                << "\n";
            ++count;
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}

quint64 dumpRegion(HANDLE process, quint64 base, quint64 size, const QString& filePath) {
    std::ofstream out(narrow(filePath), std::ios::binary);
    if (!out) {
        return 0;
    }

    constexpr SIZE_T chunkSize = 0x10000;
    std::vector<char> buffer(chunkSize);
    quint64 totalRead = 0;
    for (quint64 offset = 0; offset < size; offset += chunkSize) {
        const SIZE_T todo = static_cast<SIZE_T>(std::min<quint64>(chunkSize, size - offset));
        SIZE_T read = 0;
        if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + offset), buffer.data(), todo, &read) && read > 0) {
            out.write(buffer.data(), static_cast<std::streamsize>(read));
            totalRead += read;
            if (read < todo) {
                std::fill(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(todo - read), 0);
                out.write(buffer.data(), static_cast<std::streamsize>(todo - read));
            }
        } else {
            std::fill(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(todo), 0);
            out.write(buffer.data(), static_cast<std::streamsize>(todo));
        }
    }
    return totalRead;
}

DumpStats dumpProcessMemory(const DumpOptions& options, QString* errorText) {
    DumpStats stats;
    QString privilegeError;
    enableDebugPrivilege(&privilegeError);

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, options.pid);
    if (!process) {
        if (errorText) {
            *errorText = QStringLiteral("OpenProcess failed: %1").arg(winErrorText(GetLastError()));
        }
        return stats;
    }

    QDir().mkpath(options.outputDir);
    stats.modules = writeModuleList(options.pid, options.outputDir);

    const QString manifestPath = QDir(options.outputDir).filePath(QStringLiteral("regions.tsv"));
    std::ofstream manifest(narrow(manifestPath), std::ios::binary);
    manifest << "file\tbase\tallocation_base\tsize\tbytes_read\tprotect\ttype\n";

    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);
    quint64 current = reinterpret_cast<quint64>(sys.lpMinimumApplicationAddress);
    const quint64 maxAddress = reinterpret_cast<quint64>(sys.lpMaximumApplicationAddress);

    int index = 0;
    while (current < maxAddress) {
        MEMORY_BASIC_INFORMATION mbi{};
        const SIZE_T got = VirtualQueryEx(process, reinterpret_cast<LPCVOID>(current), &mbi, sizeof(mbi));
        if (got == 0) {
            current += 0x10000;
            continue;
        }

        const quint64 base = reinterpret_cast<quint64>(mbi.BaseAddress);
        const quint64 next = base + mbi.RegionSize;

        const bool committed = mbi.State == MEM_COMMIT;
        const bool protectionOk = options.allReadable ? isReadable(mbi.Protect) : isWritable(mbi.Protect);
        if (committed && protectionOk && typeAllowed(mbi.Type, options)) {
            const QString fileName = QStringLiteral("region_%1_0x%2_0x%3_%4_%5.bin")
                .arg(index, 4, 10, QLatin1Char('0'))
                .arg(base, 16, 16, QLatin1Char('0'))
                .arg(static_cast<quint64>(mbi.RegionSize), 0, 16)
                .arg(typeName(mbi.Type), protectName(mbi.Protect));
            const QString filePath = QDir(options.outputDir).filePath(fileName);
            const quint64 bytesRead = dumpRegion(process, base, mbi.RegionSize, filePath);

            manifest << narrow(fileName)
                << "\t0x" << hex64(base, 16)
                << "\t0x" << hex64(reinterpret_cast<quint64>(mbi.AllocationBase), 16)
                << "\t0x" << hex64(mbi.RegionSize)
                << "\t0x" << hex64(bytesRead)
                << "\t" << narrow(protectName(mbi.Protect))
                << "\t" << narrow(typeName(mbi.Type))
                << "\n";

            ++index;
            ++stats.regions;
            stats.bytesWritten += mbi.RegionSize;
        }

        current = next > current ? next : current + 0x10000;
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    CloseHandle(process);
    return stats;
}
} // namespace

ProcDump::ProcDump(QWidget *parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);
    buildUi();
    refreshProcesses();
}

ProcDump::~ProcDump()
{}

void ProcDump::buildUi() {
    setWindowTitle(QStringLiteral("ProcDump - SC2 Memory Differ"));
    resize(1120, 720);

    auto* root = new QWidget(this);
    auto* layout = new QVBoxLayout(root);

    auto* top = new QGridLayout();
    filterEdit = new QLineEdit(root);
    filterEdit->setPlaceholderText(QStringLiteral("Filter process name/path, e.g. SC2"));
    pidEdit = new QLineEdit(root);
    pidEdit->setPlaceholderText(QStringLiteral("PID"));
    dumpDirEdit = new QLineEdit(root);
    dumpDirEdit->setText(QDir::toNativeSeparators(QDir::current().filePath(QStringLiteral("dumps/sc2_off"))));

    auto* refreshButton = new QPushButton(QStringLiteral("Refresh"), root);
    auto* browseButton = new QPushButton(QStringLiteral("Browse"), root);
    dumpButton = new QPushButton(QStringLiteral("Dump Selected Process"), root);

    top->addWidget(new QLabel(QStringLiteral("Filter")), 0, 0);
    top->addWidget(filterEdit, 0, 1, 1, 3);
    top->addWidget(refreshButton, 0, 4);
    top->addWidget(new QLabel(QStringLiteral("PID")), 1, 0);
    top->addWidget(pidEdit, 1, 1);
    top->addWidget(new QLabel(QStringLiteral("Output")), 1, 2);
    top->addWidget(dumpDirEdit, 1, 3);
    top->addWidget(browseButton, 1, 4);
    layout->addLayout(top);

    auto* options = new QGroupBox(QStringLiteral("Dump Scope"), root);
    auto* optionLayout = new QGridLayout(options);
    rwOnlyCheck = new QCheckBox(QStringLiteral("Writable/read-write regions only (recommended for SC2 diff)"), options);
    rwOnlyCheck->setChecked(true);
    allReadableCheck = new QCheckBox(QStringLiteral("All readable regions"), options);
    includePrivateCheck = new QCheckBox(QStringLiteral("MEM_PRIVATE"), options);
    includeMappedCheck = new QCheckBox(QStringLiteral("MEM_MAPPED"), options);
    includeImageCheck = new QCheckBox(QStringLiteral("MEM_IMAGE"), options);
    includePrivateCheck->setChecked(true);
    includeMappedCheck->setChecked(true);
    includeImageCheck->setChecked(true);
    optionLayout->addWidget(rwOnlyCheck, 0, 0, 1, 2);
    optionLayout->addWidget(allReadableCheck, 0, 2);
    optionLayout->addWidget(includePrivateCheck, 1, 0);
    optionLayout->addWidget(includeMappedCheck, 1, 1);
    optionLayout->addWidget(includeImageCheck, 1, 2);
    optionLayout->addWidget(dumpButton, 1, 3);
    layout->addWidget(options);

    processTable = new QTableWidget(root);
    processTable->setColumnCount(3);
    processTable->setHorizontalHeaderLabels({ QStringLiteral("PID"), QStringLiteral("Process"), QStringLiteral("Path") });
    processTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    processTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    processTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    processTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    processTable->setSelectionMode(QAbstractItemView::SingleSelection);
    processTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(processTable, 1);

    logEdit = new QTextEdit(root);
    logEdit->setReadOnly(true);
    logEdit->setMinimumHeight(140);
    layout->addWidget(logEdit);

    setCentralWidget(root);

    connect(refreshButton, &QPushButton::clicked, this, &ProcDump::refreshProcesses);
    connect(browseButton, &QPushButton::clicked, this, &ProcDump::chooseDumpDirectory);
    connect(dumpButton, &QPushButton::clicked, this, &ProcDump::dumpSelectedProcess);
    connect(filterEdit, &QLineEdit::textChanged, this, &ProcDump::refreshProcesses);
    connect(allReadableCheck, &QCheckBox::toggled, this, [this](bool checked) {
        rwOnlyCheck->setChecked(!checked);
    });
    connect(rwOnlyCheck, &QCheckBox::toggled, this, [this](bool checked) {
        allReadableCheck->setChecked(!checked);
    });
    connect(processTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        const unsigned long pid = selectedPid();
        if (pid != 0) {
            pidEdit->setText(QString::number(pid));
        }
    });

    appendLog(QStringLiteral("Run as administrator for protected or elevated targets."));
    appendLog(QStringLiteral("Recommended capture set: off -> on -> off2, using writable regions."));
}

void ProcDump::refreshProcesses() {
    if (!processTable) {
        return;
    }

    const QString filter = filterEdit ? filterEdit->text().trimmed() : QString();
    const auto rows = enumerateProcesses();
    processTable->setRowCount(0);

    for (const ProcessRow& process : rows) {
        const QString haystack = process.name + QLatin1Char(' ') + process.path;
        if (!filter.isEmpty() && !haystack.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        const int row = processTable->rowCount();
        processTable->insertRow(row);
        auto* pidItem = new QTableWidgetItem(QString::number(process.pid));
        pidItem->setData(Qt::UserRole, static_cast<qulonglong>(process.pid));
        processTable->setItem(row, 0, pidItem);
        processTable->setItem(row, 1, new QTableWidgetItem(process.name));
        processTable->setItem(row, 2, new QTableWidgetItem(process.path));
    }

    appendLog(QStringLiteral("Process list refreshed: %1 visible").arg(processTable->rowCount()));
}

void ProcDump::chooseDumpDirectory() {
    const QString selected = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose Dump Directory"), dumpDirEdit->text());
    if (!selected.isEmpty()) {
        dumpDirEdit->setText(QDir::toNativeSeparators(selected));
    }
}

unsigned long ProcDump::selectedPid() const {
    const auto items = processTable ? processTable->selectedItems() : QList<QTableWidgetItem*>{};
    if (!items.isEmpty()) {
        return items.first()->data(Qt::UserRole).toULongLong();
    }
    return pidEdit ? pidEdit->text().trimmed().toULong() : 0;
}

void ProcDump::dumpSelectedProcess() {
    DumpOptions options;
    options.pid = selectedPid();
    options.outputDir = dumpDirEdit->text().trimmed();
    options.rwOnly = rwOnlyCheck->isChecked();
    options.allReadable = allReadableCheck->isChecked();
    options.includeImage = includeImageCheck->isChecked();
    options.includeMapped = includeMappedCheck->isChecked();
    options.includePrivate = includePrivateCheck->isChecked();

    if (options.pid == 0) {
        QMessageBox::warning(this, QStringLiteral("ProcDump"), QStringLiteral("Select a process or enter a PID."));
        return;
    }
    if (options.outputDir.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ProcDump"), QStringLiteral("Choose an output directory."));
        return;
    }

    dumpButton->setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    appendLog(QStringLiteral("Dump started: pid=%1 output=%2 scope=%3")
        .arg(options.pid)
        .arg(options.outputDir)
        .arg(options.allReadable ? QStringLiteral("all readable") : QStringLiteral("writable only")));

    QString error;
    const DumpStats stats = dumpProcessMemory(options, &error);

    QApplication::restoreOverrideCursor();
    dumpButton->setEnabled(true);

    if (!error.isEmpty()) {
        appendLog(error);
        QMessageBox::critical(this, QStringLiteral("ProcDump"), error);
        return;
    }

    appendLog(QStringLiteral("Dump finished: regions=%1 modules=%2 bytes=0x%3")
        .arg(stats.regions)
        .arg(stats.modules)
        .arg(stats.bytesWritten, 0, 16));
    appendLog(QStringLiteral("Wrote regions.tsv and modules.tsv under %1").arg(options.outputDir));
}

void ProcDump::appendLog(const QString& message) {
    if (!logEdit) {
        return;
    }
    logEdit->append(QStringLiteral("[%1] %2")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")))
        .arg(message));
}

