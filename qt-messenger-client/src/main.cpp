#include "mainwindow.h"
#include <QApplication>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    //Создаём главное окно и задаём ему заголовок
    MainWindow w;
    w.setWindowTitle(QStringLiteral("Мой Мессенджер"));
    w.show();

    return app.exec();
}