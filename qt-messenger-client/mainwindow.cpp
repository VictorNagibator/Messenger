#include "mainwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QInputDialog>
#include <QLabel>
#include <QDateTime>
#include <QDebug>
#include "crypto.h"

const QString MainWindow::AES_KEY = QString::fromUtf8("01234567890123456789012345678901");

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    // Сетевой сокет
    socket = new QTcpSocket(this);
    socket->connectToHost("127.0.0.1", 12345);
    connect(socket, &QTcpSocket::readyRead, this, &MainWindow::onSocketReadyRead);
    connect(socket, &QTcpSocket::connected, this, [](){
        qDebug() << "[socket] connected";
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [](QAbstractSocket::SocketError e){
        qDebug() << "[socket] error" << e;
    });

    // Стек страниц
    stack = new QStackedWidget(this);
    setCentralWidget(stack);

    // ------- Login Page -------
    pageLogin = new QWidget(this);
    {
        auto *lay = new QVBoxLayout(pageLogin);
        lay->addWidget(new QLabel("<h3>Login / Register</h3>", pageLogin));
        usernameEdit = new QLineEdit(pageLogin);
        usernameEdit->setPlaceholderText("Username");
        lay->addWidget(usernameEdit);
        passwordEdit = new QLineEdit(pageLogin);
        passwordEdit->setEchoMode(QLineEdit::Password);
        passwordEdit->setPlaceholderText("Password");
        lay->addWidget(passwordEdit);

        auto *h = new QHBoxLayout();
        loginButton    = new QPushButton("Login",    pageLogin);
        registerButton = new QPushButton("Register", pageLogin);
        h->addWidget(loginButton);
        h->addWidget(registerButton);
        lay->addLayout(h);

        connect(loginButton,    &QPushButton::clicked, this, &MainWindow::onLogin);
        connect(registerButton, &QPushButton::clicked, this, &MainWindow::onRegister);
    }
    stack->addWidget(pageLogin);

    // ------- Chats Page -------
    pageChats = new QWidget(this);
    {
        auto *v = new QVBoxLayout(pageChats);

        auto *top = new QHBoxLayout();
        newChatButton  = new QPushButton("New Chat",  pageChats);
        newGroupButton = new QPushButton("New Group", pageChats);
        logoutButton   = new QPushButton("Logout",    pageChats);
        top->addWidget(newChatButton);
        top->addWidget(newGroupButton);
        top->addStretch();
        top->addWidget(logoutButton);
        v->addLayout(top);

        connect(newChatButton,  &QPushButton::clicked, this, [this](){
            bool ok;
            QString name = QInputDialog::getText(this,"New Chat",
                              "Enter peer username:", QLineEdit::Normal, {}, &ok);
            if (!ok || name.isEmpty()) return;
            pendingPeerName = name;
            expectingUserId = true;
            sendCmd("GET_USER_ID " + name);
        });
        connect(newGroupButton, &QPushButton::clicked, this, [this](){
            bool ok;
            // 1) спрашиваем название
            pendingGroupName = QInputDialog::getText(
                this, "New Group", "Group name:", QLineEdit::Normal, {}, &ok);
            if (!ok || pendingGroupName.isEmpty()) return;

            // 2) спрашиваем список юзеров
            QString members = QInputDialog::getText(
                this, "New Group", "Usernames (space-separated):",
                QLineEdit::Normal, {}, &ok);
            if (!ok) return;

            // 3) запомним, что собираем группу
            pendingGroupNames = members.split(' ', Qt::SkipEmptyParts);
            pendingGroupIds.clear();
            creatingGroup = true;

            // 4) запросим ID первого пользователя
            expectingUserId = true;
            sendCmd("GET_USER_ID " + pendingGroupNames.first());
        });

        auto *h = new QHBoxLayout();
        chatsList = new QListWidget(pageChats);
        h->addWidget(chatsList, 1);

        auto *r = new QVBoxLayout();
        chatView = new QTextEdit(pageChats);
        chatView->setReadOnly(true);
        r->addWidget(chatView, 1);
        auto *sh = new QHBoxLayout();
        messageEdit = new QLineEdit(pageChats);
        sendButton  = new QPushButton("Send", pageChats);
        sh->addWidget(messageEdit, 1);
        sh->addWidget(sendButton);
        r->addLayout(sh);

        h->addLayout(r, 2);
        v->addLayout(h);

        connect(logoutButton,  &QPushButton::clicked, this, &MainWindow::onLogout);
        connect(chatsList,     &QListWidget::itemClicked, this, &MainWindow::onChatSelected);
        connect(sendButton,    &QPushButton::clicked, this, &MainWindow::onSend);
    }
    stack->addWidget(pageChats);

    // show login initially
    stack->setCurrentWidget(pageLogin);
}

MainWindow::~MainWindow() {}

void MainWindow::sendCmd(const QString &cmd) {
    qDebug() << "[sendCmd]" << cmd;
    socket->write(cmd.toUtf8() + "\n");
}

// --- Slots ---

void MainWindow::onRegister() {
    QString u = usernameEdit->text(), p = passwordEdit->text();
    if (u.isEmpty() || p.isEmpty()) {
        QMessageBox::warning(this,"Error","Enter username & password");
        return;
    }
    sendCmd(QString("REGISTER %1 %2").arg(u,p));
}

void MainWindow::onLogin() {
    QString u = usernameEdit->text(), p = passwordEdit->text();
    qDebug() << "[onLogin]" << u << p;
    if (u.isEmpty() || p.isEmpty()) {
        QMessageBox::warning(this,"Error","Enter username & password");
        return;
    }
    sendCmd(QString("LOGIN %1 %2").arg(u,p));
}

void MainWindow::onLogout() {
    myUserId = -1;
    currentChatId = -1;
    userMap.clear();
    chatsList->clear();
    chatView->clear();
    usernameEdit->clear();
    passwordEdit->clear();
    stack->setCurrentWidget(pageLogin);
}

void MainWindow::onChatSelected() {
    auto *it = chatsList->currentItem();
    if (!it) return;
    currentChatId = it->data(Qt::UserRole).toInt();
    chatView->clear();
    sendCmd(QString("HISTORY %1").arg(currentChatId));
}

void MainWindow::onSend() {
    if (currentChatId < 0) return;
    QString msg = messageEdit->text().trimmed();
    if (msg.isEmpty()) return;

    QByteArray bin = QByteArray::fromStdString(encrypt(msg.toStdString(),AES_KEY.toStdString()));
    QString hex = bin.toHex();
    sendCmd(QString("SEND %1 ").arg(currentChatId) + hex);

    // сразу показываем своё сообщение
    QString now  = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm");
    QString nick = userMap.value(myUserId, usernameEdit->text());
    chatView->append(QString("[%1] %2: %3").arg(now, nick, msg));
    messageEdit->clear();
}

void MainWindow::onSocketReadyRead() {
    // 1) Считаем всё, что пришло
    QByteArray data = socket->readAll();
    QStringList lines = QString::fromUtf8(data).split('\n', Qt::SkipEmptyParts);

    // 2) Обрабатываем каждую строку
    for (const QString &line : lines) {
        qDebug() << "RECV:" << line;

        if (creatingGroup && expectingUserId) {
            expectingUserId = false;
            bool okId;
            int uid = line.toInt(&okId);
            if (!okId || uid <= 0) {
                QMessageBox::warning(this, "Error",
                    "User \"" + pendingGroupNames[pendingGroupIds.size()] + "\" not found");
                creatingGroup = false;
                continue;
            }
            // сохраняем и, если остались, запрашиваем следующего
            pendingGroupIds.append(uid);
            if (pendingGroupIds.size() < pendingGroupNames.size()) {
                expectingUserId = true;
                sendCmd("GET_USER_ID " + pendingGroupNames.at(pendingGroupIds.size()));
            } else {
                // все ID собраны — отправляем единоразово CREATE_CHAT 1
                QString cmd = "CREATE_CHAT 1 " + pendingGroupName;
                for (int id : pendingGroupIds)
                    cmd += " " + QString::number(id);
                sendCmd(cmd);
                creatingGroup = false;
            }
            continue;
        }

        // 1) Ответ на GET_USER_ID
        if (expectingUserId) {
            expectingUserId = false;
            bool ok; int peerId = line.toInt(&ok);
            if (ok && peerId > 0) {
                sendCmd(QString("CREATE_CHAT 0 %1 %2").arg(myUserId).arg(peerId));
            } else {
                QMessageBox::warning(this,"Error",
                    "User \"" + pendingPeerName + "\" not found");
            }
            continue;
        }

        if (line.startsWith("NEW_CHAT ")) {
            // Уберём префикс и финальный ';'
            QString rest = line.mid(9).trimmed();          // после "NEW_CHAT "
            if (rest.endsWith(';')) rest.chop(1);

            // rest == "<chat_id>:<is_group>:<chatName>"
            QStringList p = rest.split(':');
            int cid     = p[0].toInt();
            bool isg    = (p[1] == "1");
            QString name= p[2];

            // Добавляем в список
            QString disp = isg
                ? QString("Group: %1").arg(name)
                : QString("Private: %1").arg(name);
            auto *item = new QListWidgetItem(disp, chatsList);
            item->setData(Qt::UserRole, cid);
            chatsList->addItem(item);

            // сразу подписываемся и открываем историю
            onChatSelected();
            continue;
        }

        // 2) REGISTER/LOGIN OK
        if (line.startsWith("OK ")) {
            myUserId = line.split(' ')[1].toInt();
            stack->setCurrentWidget(pageChats);
            sendCmd("LIST_CHATS");
            continue;
        }

        // 3) Ошибки
        if (line.startsWith("ERROR")) {
            QMessageBox::warning(this,"Error", line);
            continue;
        }

        // 4) Список чатов
        if (line.startsWith("CHATS ")) {
            chatsList->clear();
                for (const QString &it : line.mid(6).split(';', Qt::SkipEmptyParts)) {
                    auto p = it.split(':');
                    int cid        = p[0].toInt();
                    bool isg       = (p[1] == "1");
                    QString name   = p[2];
                    QString disp   = isg
                        ? QString("Group: %1").arg(name)
                        : QString("Private: %1").arg(name);
                    auto *item = new QListWidgetItem(disp);
                    item->setData(Qt::UserRole, cid);
                    item->setData(Qt::UserRole + 1, name);
                    chatsList->addItem(item);
                }
            if (chatsList->count() > 0) onChatSelected();
            continue;
        }

        // 5) Ответ на CREATE_CHAT (число)
        bool okNum; int v = line.toInt(&okNum);
        if (okNum && v > 0) {
            sendCmd("LIST_CHATS");
            continue;
        }

        // 6) Любая строка истории
        if (line.startsWith("[")) {
            chatView->append(line);
            continue;
        }

        // всё остальное игнорим
    }
}