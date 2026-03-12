#include "ZeratulRelicScanner.h"
#include <QtWidgets/QApplication>

// 请求管理员权限（需要在链接器中添加manifest或使用下面的pragma）
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    ZeratulRelicScanner window;
    window.show();
    return app.exec();
}
