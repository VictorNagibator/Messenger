#pragma once

#include <QMainWindow>
#include <QSslSocket>
#include <QStackedWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QTextEdit>
#include <QTimer>
#include <QMap>
#include <QHash>
#include <QLabel>

// Тип записи в чате — либо сообщение, либо системное событие
struct ChatEntry {
    enum Type { Message, Event } type;
    QString date;      // "[2025-05-20 16:30]"
    QString author;    // для Message — имя, для Event — пусто
    QString text;      // содержимое или текст события
    int     id = -1;   // для Message — msg_id, для Event не используется
};

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
    void onChatsListContextMenu(const QPoint &pt);
    void redrawChatFromCache();

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
    QSslSocket *socket;
    int             myUserId       = -1;
    QString         myUsername;
    int             currentChatId  = -1;
    bool            expectingUserId = false;
    bool            creatingGroup = false;
    QString         pendingPeerName;
    QMap<int,QString> userMap;

    QLabel      *userLabel;

    // Для контекстного меню удаления
    // (id сообщений у нас выводится (id=...))
    // и для названий чатов:
    QHash<int,QString> cidMap;

    QString     pendingGroupName;
    QStringList pendingGroupNames;    // исходные юзернеймы
    QVector<int> pendingGroupIds;     // resolved user IDs

    QMap<int,int> blockToMsgId;  
    QHash<int, QVector<ChatEntry>> cache; // chat_id → история

    ChatEntry lastMessage;

    void sendCmd(const QString &cmd);
    void appendHtmlLine(const QString &html);
};