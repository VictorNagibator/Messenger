#include "mainwindow.h"
#include <QApplication>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    // 2) Создаём главное окно и задаём ему заголовок
    MainWindow w;
    w.setWindowTitle(QStringLiteral("Мой Мессенджер"));  // ваше название
    w.show();

    return app.exec();
}