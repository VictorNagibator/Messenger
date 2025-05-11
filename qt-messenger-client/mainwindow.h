#pragma once
#include <QMainWindow>
#include <QTcpSocket>
#include <QStackedWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QTextEdit>
#include <QLabel>

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onRegister();
    void onLogin();
    void onRefreshChats();
    void onLogout();
    void onChatSelected();
    void onSend();
    void onSocketReadyRead();

private:
    // UI
    QStackedWidget *stack;
    // Login page
    QWidget      *pageLogin;
    QLineEdit    *usernameEdit;
    QLineEdit    *passwordEdit;
    QPushButton  *loginButton;
    QPushButton  *registerButton;
    // Chats page
    QWidget      *pageChats;
    QPushButton  *refreshButton;
    QPushButton  *logoutButton;
    QListWidget  *chatsList;
    QTextEdit    *chatView;
    QLineEdit    *messageEdit;
    QPushButton  *sendButton;

    // Network
    QTcpSocket   *socket;
    int           myUserId = -1;
    int           currentChatId = -1;

    void sendCmd(const QString &cmd);
    static const QString AES_KEY;
};
