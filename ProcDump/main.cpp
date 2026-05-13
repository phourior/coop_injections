#include "ProcDump.h"
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    ProcDump window;
    window.show();
    return app.exec();
}
