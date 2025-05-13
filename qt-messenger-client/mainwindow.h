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
#include <QHash>

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
    void onChatViewContextMenu(const QPoint &pt);

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
    QString         myUsername;
    int             currentChatId  = -1;
    bool            expectingUserId = false;
    bool            creatingGroup = false;
    QString         pendingPeerName;
    QMap<int,QString> userMap;

    // Для контекстного меню удаления
    // (id сообщений у нас выводится (id=...))
    // и для названий чатов:
    QHash<int,QString> cidMap;

    QString     pendingGroupName;
    QStringList pendingGroupNames;    // исходные юзернеймы
    QVector<int> pendingGroupIds;     // resolved user IDs

    void sendCmd(const QString &cmd);
    static const QString AES_KEY;
};