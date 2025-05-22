#pragma once

#include <QMainWindow>
#include <QSslSocket>
#include <QStackedWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QTextEdit>
#include <QLabel>
#include <QMessageBox>
#include <QInputDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDateTime>
#include <QMenu>
#include <QRegularExpression>
#include <QFile>
#include <QCoreApplication>
#include <QNetworkProxy>

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onRegister(); //при нажатии «Зарегистрироваться»
    void onLogin(); //при нажатии «Войти»
    void onLogout(); //при нажатии «Выйти из профиля»
    void onChatSelected(); //при выборе чата в списке
    void onSend(); //при нажатии «Отправить»
    void onSocketReadyRead(); //данные от сервера готовы к чтению
    void onChatViewContextMenu(const QPoint &pt); //контекстное меню внутри окна чата
    void onChatsListContextMenu(const QPoint &pt); //контекстное меню для списка чатов
    void redrawChatFromCache(); //перерисовать историю из локального кэша

private:
    //UI элементы
    QStackedWidget *stack; //переключатель страниц (login <-> chats)

    //Страница логина
    QWidget *pageLogin;
    QLineEdit *usernameEdit; //ввод имени
    QLineEdit *passwordEdit; //ввод пароля
    QPushButton *loginButton; //кнопка «Войти»
    QPushButton *registerButton; //кнопка «Зарегистрироваться»

    //Страница чатов
    QWidget *pageChats;
    QLabel *userLabel; //отображает «Пользователь: <имя>»
    QPushButton *newChatButton; //кнопка «Новый личный чат»
    QPushButton *newGroupButton; //кнопка «Новая группа»
    QListWidget *chatsList; //список доступных чатов
    QTextEdit *chatView; //окно истории сообщений
    QLineEdit *messageEdit; //ввод нового сообщения
    QPushButton *sendButton; //кнопка «Отправить»

    //Сеть и протокол
    QSslSocket *socket; //шифрованный TCP-сокет (SSL)
    int myUserId = -1; //идентификатор текущего пользователя
    QString myUsername; //его имя
    int currentChatId = -1; //выбранный chat_id

    //Флаги по многошаговым операциям
    bool expectingUserId = false; //ждем ID пользователя (для CREATE_CHAT)
    bool creatingGroup = false; //в процессе сборки группового чата
    QString pendingPeerName; //временно: имя собеседника
    QString pendingGroupName; // временно: имя создаваемой группы
    QStringList pendingGroupNames; //имена участников группы
    QVector<int> pendingGroupIds; //уже готовые ID пользователей, добавляемых в группу

    //Запись чата: либо пользовательское сообщение, либо событие 
    struct ChatEntry {
        enum Type { Message, Event } type;
        QString date; //временная метка
        QString author;//для Message — имя пользователя, для Event — пустая строка
        QString text; //текст сообщения или описание события
        int id = -1; //message_id, для Event не используется
    };

    //Локальный кэш истории: для каждого chat_id — вектор ChatEntry
    QHash<int, QVector<ChatEntry>> cache;

    //Вспомогательные методы
    void sendCmd(const QString &cmd); //отправляет команду серверу по сокету
    void appendHtmlLine(const QString &html); //вставляет HTML в chatView

    //Контекстные меню
    //Для удаления конкретного сообщения по позиции в chatView
    QMap<int,int> blockToMsgId;  //blockNumber -> msg_id
    // Для сохранения имён групп/чатов
    QHash<int,QString> cidMap; // chat_id -> отображаемое имя чата
};
