#pragma once

#include <QMainWindow>
#include <QTcpSocket>
#include <QStackedWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QTextEdit>
#include <QTimer>
#include <QMap>

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onRegister();
    void onLogin();
    void onLogout();
    void onChatSelected();
    void onSend();
    void onSocketReadyRead();

private:
    // UI
    QStackedWidget *stack;
    QWidget        *pageLogin;
    QLineEdit      *usernameEdit;
    QLineEdit      *passwordEdit;
    QPushButton    *loginButton;
    QPushButton    *registerButton;

    QWidget        *pageChats;
    QPushButton    *logoutButton;
    QPushButton    *newChatButton;
    QPushButton    *newGroupButton;
    QListWidget    *chatsList;
    QTextEdit      *chatView;
    QLineEdit      *messageEdit;
    QPushButton    *sendButton;

    // Network
    QTcpSocket     *socket;
    int             myUserId       = -1;
    int             currentChatId  = -1;
    bool            expectingUserId = false;
    QString         pendingPeerName;
    QMap<int,QString> userMap;

    QString     pendingGroupName;
    QStringList pendingGroupNames;    // исходные юзернеймы
    QVector<int> pendingGroupIds;     // resolved user IDs
    bool        creatingGroup = false;

    void sendCmd(const QString &cmd);
    static const QString AES_KEY;
};
