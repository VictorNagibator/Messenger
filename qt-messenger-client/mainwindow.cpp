#include "mainwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
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

    // Стек страниц
    stack = new QStackedWidget(this);
    setCentralWidget(stack);

    // --- Страница логина ---
    pageLogin = new QWidget(this);
    {
        auto *layout = new QVBoxLayout(pageLogin);
        layout->addWidget(new QLabel("Login / Register", pageLogin));
        usernameEdit = new QLineEdit(pageLogin);
        usernameEdit->setPlaceholderText("Username");
        layout->addWidget(usernameEdit);
        passwordEdit = new QLineEdit(pageLogin);
        passwordEdit->setEchoMode(QLineEdit::Password);
        passwordEdit->setPlaceholderText("Password");
        layout->addWidget(passwordEdit);

        auto *hbox = new QHBoxLayout();
        loginButton = new QPushButton("Login", pageLogin);
        registerButton = new QPushButton("Register", pageLogin);
        hbox->addWidget(loginButton);
        hbox->addWidget(registerButton);
        layout->addLayout(hbox);

        connect(loginButton,   &QPushButton::clicked, this, &MainWindow::onLogin);
        connect(registerButton,&QPushButton::clicked, this, &MainWindow::onRegister);
    }
    stack->addWidget(pageLogin);

    // --- Страница чатов ---
    pageChats = new QWidget(this);
    {
        auto *vlay = new QVBoxLayout(pageChats);
        auto *topBar = new QHBoxLayout();
        refreshButton = new QPushButton("Refresh Chats", pageChats);
        logoutButton  = new QPushButton("Logout", pageChats);
        topBar->addWidget(refreshButton);
        topBar->addWidget(logoutButton);
        vlay->addLayout(topBar);

        auto *mainHb = new QHBoxLayout();
        chatsList  = new QListWidget(pageChats);
        mainHb->addWidget(chatsList);

        auto *chatVlay = new QVBoxLayout();
        chatView   = new QTextEdit(pageChats);
        chatView->setReadOnly(true);
        chatVlay->addWidget(chatView);
        auto *sendHb = new QHBoxLayout();
        messageEdit = new QLineEdit(pageChats);
        sendButton  = new QPushButton("Send", pageChats);
        sendHb->addWidget(messageEdit);
        sendHb->addWidget(sendButton);
        chatVlay->addLayout(sendHb);

        mainHb->addLayout(chatVlay);
        vlay->addLayout(mainHb);

        connect(refreshButton, &QPushButton::clicked, this, &MainWindow::onRefreshChats);
        connect(logoutButton,  &QPushButton::clicked, this, &MainWindow::onLogout);
        connect(chatsList,     &QListWidget::itemClicked, this, &MainWindow::onChatSelected);
        connect(sendButton,    &QPushButton::clicked, this, &MainWindow::onSend);
    }
    stack->addWidget(pageChats);

    // Показываем страницу логина
    stack->setCurrentWidget(pageLogin);
}

MainWindow::~MainWindow() {
}

void MainWindow::sendCmd(const QString &cmd) {
    QByteArray data = cmd.toUtf8() + "\n";
    socket->write(data);
}

void MainWindow::onRegister() {
    QString u = usernameEdit->text(), p = passwordEdit->text();
    if (u.isEmpty() || p.isEmpty()) {
        QMessageBox::warning(this, "Error", "Enter username and password");
        return;
    }
    sendCmd(QString("REGISTER %1 %2").arg(u, p));
}

void MainWindow::onLogin() {
    QString u = usernameEdit->text(), p = passwordEdit->text();
    if (u.isEmpty() || p.isEmpty()) {
        QMessageBox::warning(this, "Error", "Enter username and password");
        return;
    }
    sendCmd(QString("LOGIN %1 %2").arg(u, p));
}

void MainWindow::onRefreshChats() {
    if (myUserId > 0) sendCmd("LIST_CHATS");
}

void MainWindow::onLogout() {
    myUserId = -1; currentChatId = -1;
    usernameEdit->clear(); passwordEdit->clear();
    chatView->clear(); chatsList->clear();
    stack->setCurrentWidget(pageLogin);
}

void MainWindow::onChatSelected() {
    QString t = chatsList->currentItem()->text();
    int cid = t.section(':',0,0).toInt();
    currentChatId = cid;
    chatView->clear();
    sendCmd(QString("HISTORY %1").arg(cid));
}

void MainWindow::onSend() {
    if (currentChatId < 0) return;
    QString msg = messageEdit->text();
    if (msg.isEmpty()) return;
    QByteArray enc = QByteArray::fromStdString(encrypt(msg.toStdString(), AES_KEY.toStdString()));
    sendCmd(QString("SEND %1 ").arg(currentChatId) + QString(enc));
    messageEdit->clear();
}

void MainWindow::onSocketReadyRead() {
    while (socket->canReadLine()) {
        QString line = socket->readLine().trimmed();
        qDebug() << "RECV:" << line;
        if (line.startsWith("OK ")) {
            // OK <id>
            int id = line.split(' ')[1].toInt();
            myUserId = id;
            QMessageBox::information(this, "Logged In", "User ID = " + QString::number(id));
            stack->setCurrentWidget(pageChats);
            sendCmd("LIST_CHATS");
        }
        else if (line.startsWith("ERROR USER_EXISTS")) {
            QMessageBox::warning(this, "Error", "User already exists");
        }
        else if (line.startsWith("ERROR")) {
            QMessageBox::warning(this, "Error", line);
        }
        else if (line.startsWith("CHATS ")) {
            chatsList->clear();
            QStringList items = line.mid(6).split(';', Qt::SkipEmptyParts);
            for (const auto &it : items) {
                QStringList p = it.split(':');
                chatsList->addItem(p[0] + ": " + p[1]);
            }
        }
        else if (line.startsWith("HISTORY ")) {
            chatView->append(line.mid(8));
        }
        else {
            chatView->append(line);
        }
    }
}
