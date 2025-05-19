#include "mainwindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QInputDialog>
#include <QLabel>
#include <QDateTime>
#include <QDebug>
#include <QMenu>
#include <QRegularExpression>
#include <QTextBlock>
#include <QCoreApplication>
#include <QFile>
#include <QSslConfiguration>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    QString host = "127.0.0.1";
    int     port = 12345;

    QString cfgPath = "./config.ini";

    QFile f(cfgPath);
    if (f.open(QIODevice::ReadOnly|QIODevice::Text)) {
        QTextStream in(&f);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.startsWith("host="))
            host = line.mid(sizeof("host=")-1).trimmed();
            else if (line.startsWith("port="))
            port = line.mid(sizeof("port=")-1).toInt();
        }
    }

    // 1) Определяем путь к CA (server.crt)
    QString caPath = QCoreApplication::applicationDirPath() + "/server.crt";
    qDebug() << "Loading CA from" << caPath << "exists?" << QFile::exists(caPath);

    // 2) Читаем файл в память и парсим сертификат PEM
    QList<QSslCertificate> caCerts;
    QFile fCa(caPath);
    if (fCa.open(QIODevice::ReadOnly)) {
        QByteArray pem = fCa.readAll();
        caCerts = QSslCertificate::fromData(pem, QSsl::Pem);
        fCa.close();
    }

    if (caCerts.isEmpty()) {
        qWarning() << "Failed to load any CA certificates!";
    } else {
        qDebug() << "Loaded" << caCerts.count() << "CA cert(s)";
    }

    // 3) Настраиваем socket
    socket = new QSslSocket(this);
    socket->setPeerVerifyMode(QSslSocket::VerifyNone);

    // Подменяем конфигурацию, добавляем наш CA
    QSslConfiguration cfg = socket->sslConfiguration();
    cfg.setCaCertificates(caCerts);
    socket->setSslConfiguration(cfg);

    // 1) Логгируем переходы состояний (TCP → TLS → готово)
    connect(socket, &QAbstractSocket::stateChanged, this, [](QAbstractSocket::SocketState st){
        qDebug() << "Socket state changed to" << st;
    });

    // 2) Логгируем все SSL‑ошибки с подробностями
    connect(socket,
            static_cast<void(QSslSocket::*)(const QList<QSslError>&)>(&QSslSocket::sslErrors),
            this,
            [this](const QList<QSslError> &errs){
                for (auto &e : errs)
                    qWarning() << "SSL error:" << e.errorString();
                socket->ignoreSslErrors();  // чтобы рукопожатие всё-таки продолжилось
            });

    // 3) Логгируем любые сетевые ошибки
    connect(socket, &QAbstractSocket::errorOccurred, this, [](QAbstractSocket::SocketError err){
        qWarning() << "Network error:" << err;
    });

    // 4) Логгируем успешный конец TLS‑рукопожатия
    connect(socket, &QSslSocket::encrypted, this, [](){
        qDebug() << "TLS handshake completed";
    });
    connect(socket, &QSslSocket::readyRead, this, &MainWindow::onSocketReadyRead);

    socket->connectToHostEncrypted(host, port);

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

        // Верхняя панель: New Chat / New Group / Logout
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
            pendingGroupName = QInputDialog::getText(
                this, "New Group", "Group name:", QLineEdit::Normal, {}, &ok);
            if (!ok || pendingGroupName.isEmpty()) return;
            QString members = QInputDialog::getText(
                this, "New Group", "Usernames (space-separated):",
                QLineEdit::Normal, {}, &ok);
            if (!ok) return;
            pendingGroupNames = members.split(' ', Qt::SkipEmptyParts);
            pendingGroupIds.clear();
            creatingGroup = true;
            expectingUserId = true;
            sendCmd("GET_USER_ID " + pendingGroupNames.first());
        });
        connect(logoutButton, &QPushButton::clicked, this, &MainWindow::onLogout);

        auto *h = new QHBoxLayout();
        chatsList = new QListWidget(pageChats);
        h->addWidget(chatsList, 1);

        auto *r = new QVBoxLayout();

        chatView = new QTextEdit(pageChats);
        chatView->setReadOnly(true);

        chatView->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(chatView, &QWidget::customContextMenuRequested,
                this,    &MainWindow::onChatViewContextMenu);

        r->addWidget(chatView, 1);

        auto *sh = new QHBoxLayout();
        messageEdit = new QLineEdit(pageChats);
        sendButton  = new QPushButton("Send", pageChats);
        sh->addWidget(messageEdit, 1);
        sh->addWidget(sendButton);
        r->addLayout(sh);

        h->addLayout(r, 2);
        v->addLayout(h);

        connect(chatsList, &QListWidget::itemClicked, this, &MainWindow::onChatSelected);
        connect(sendButton, &QPushButton::clicked,    this, &MainWindow::onSend);
    }
    stack->addWidget(pageChats);

    // Показываем страницу логина
    stack->setCurrentWidget(pageLogin);
}

MainWindow::~MainWindow() {}

// Отправка команды
void MainWindow::sendCmd(const QString &cmd) {
    qDebug() << "[sendCmd]" << cmd;
    socket->write(cmd.toUtf8() + "\n");
}

void MainWindow::redrawChatFromCache() {
    chatView->clear();
    for (const Message &m : cache[currentChatId]) {
        chatView->append(
          QString("[%1] %2: %3")
            .arg(m.date, m.author, m.text)
        );
    }
}

void MainWindow::onChatViewContextMenu(const QPoint &pt) {
    QTextCursor cursor = chatView->cursorForPosition(pt);
    QTextBlock  block  = cursor.block();                    // теперь работает
    int blockNo = block.blockNumber();

    // дальше вы можете по номеру блока смотреть cache[currentChatId][blockNo].id
    // и удалять именно то сообщение.
    QMenu menu;
    QAction *delMe  = menu.addAction("Delete for me");
    QAction *delAll = menu.addAction("Delete for all");
    QAction *act = menu.exec(QCursor::pos());
    if (!act) return;
    int msgId = cache[currentChatId][blockNo].id;
    if (act == delMe) {
        sendCmd(QString("DELETE %1").arg(msgId));
    } else if (act == delAll) {
        sendCmd(QString("DELETE_GLOBAL %1").arg(msgId));
    }
}

// Слот: зарегистрироваться
void MainWindow::onRegister() {
    QString u = usernameEdit->text(), p = passwordEdit->text();
    if (u.isEmpty() || p.isEmpty()) {
        QMessageBox::warning(this,"Error","Enter username & password");
        return;
    }
    sendCmd(QString("REGISTER %1 %2").arg(u,p));
}

// Слот: залогиниться
void MainWindow::onLogin() {
    QString u = usernameEdit->text(), p = passwordEdit->text();
    if (u.isEmpty() || p.isEmpty()) {
        QMessageBox::warning(this,"Error","Enter username & password");
        return;
    }
    sendCmd(QString("LOGIN %1 %2").arg(u,p));
    myUsername = u;
}

// Слот: разлогиниться
void MainWindow::onLogout() {
    myUserId = -1; currentChatId = -1;
    chatsList->clear();
    chatView->clear();
    usernameEdit->clear();
    passwordEdit->clear();

    cache.clear();

    stack->setCurrentWidget(pageLogin);
}

void MainWindow::onChatSelected() {
    auto *it = chatsList->currentItem();
    if (!it) return;
    currentChatId = it->data(Qt::UserRole).toInt();
    chatView->clear();

    // Если есть кэш — рисуем из него
    if (cache.contains(currentChatId)) {
        const QVector<Message> &msgs = cache[currentChatId];
        for (const Message &m : msgs) {
            // Формируем строку без «(id=...)»
            chatView->append(
                QString("[%1] %2: %3")
                    .arg(m.date, m.author, m.text)
            );
        }
        return;
    }

    // Иначе — запрашиваем у сервера
    sendCmd(QString("HISTORY %1").arg(currentChatId));
}

// Слот: отправить сообщение
void MainWindow::onSend() {
    if (currentChatId < 0) return;
    QString msg = messageEdit->text().trimmed();
    if (msg.isEmpty()) return;
    sendCmd(QString("SEND %1 ").arg(currentChatId) + msg);

    //show message
    QString now = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm");
    chatView->append(QString("[%1] %2: %3").arg(now, myUsername, msg));
    messageEdit->clear();

    lastMessage.date = now;
    lastMessage.author = myUsername;
    lastMessage.text = msg;
}

// Слот: пришли данные от сервера
void MainWindow::onSocketReadyRead() {
    QByteArray data = socket->readAll();
    QStringList lines = QString::fromUtf8(data)
                          .split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        qDebug() << "RECV:" << line;

        // 1) Ответ на GET_USER_ID (для создания чата)
        if (expectingUserId) {
            expectingUserId = false;
            bool ok; int uid = line.toInt(&ok);
            if (!ok || uid<=0) {
                QMessageBox::warning(this,"Error",
                    "User \"" + pendingPeerName + "\" not found");
            } else {
                // если мы в процессе создания группы
                if (creatingGroup) {
                    pendingGroupIds.append(uid);
                    if (pendingGroupIds.size() < pendingGroupNames.size()) {
                        expectingUserId = true;
                        sendCmd("GET_USER_ID " + pendingGroupNames[pendingGroupIds.size()]);
                    } else {
                        // все ID собраны, отправляем CREATE_CHAT
                        QString cmd = "CREATE_CHAT 1 " + pendingGroupName;
                        for (int x : pendingGroupIds)
                            cmd += " " + QString::number(x);
                        sendCmd(cmd);
                        creatingGroup = false;
                    }
                } else {
                    // обычный приватный чат
                    sendCmd(QString("CREATE_CHAT 0 %1")
                                .arg(uid));
                }
            }
            continue;
        }

        // 2) Повторный приватный чат
        if (line == "ERROR CHAT_EXISTS") {
            QMessageBox::information(this,"Info",
                                     "Private chat already exists.");
            continue;
        }

        // 3) Успешный логин
        if (line.startsWith("OK LOGIN")) {
            myUserId = line.split(' ')[1].toInt();
            stack->setCurrentWidget(pageChats);

            sendCmd("LIST_CHATS");
            continue;
        }

        if (line.startsWith("OK REG")) {
            QMessageBox::information(this, "Success!", "Successful sign in! Now try logging in...");
            continue;
        }

        if (line.startsWith("OK SENT")) {
            bool ok;
            int mid = line.mid(QString("OK SENT ").length()).toInt(&ok);
            if (ok) {
                lastMessage.id = mid;
                cache[currentChatId].append(lastMessage);
                redrawChatFromCache();
            }
            continue;
        }


        // 4) Новый чат
        if (line.startsWith("NEW_CHAT")) {
            sendCmd("LIST_CHATS");
            continue;
        }

        if (line.startsWith("NEW_HISTORY")) {
            auto *it = chatsList->currentItem();
            if (!it) continue;
            currentChatId = it->data(Qt::UserRole).toInt();
            if (currentChatId == line.mid(12).toInt()) {
                sendCmd("HISTORY " + QString::number(currentChatId));
            }
            continue;
        }

        // 5) Список чатов
        if (line.startsWith("CHATS")) {
            chatsList->clear();
            auto chunks = line.mid(6)
                          .split(';', Qt::SkipEmptyParts);
            for (auto &chunk : chunks) {
                auto p = chunk.split(':');
                int cid     = p[0].toInt();
                bool isg    = (p[1]=="1");
                QString name = p[2];
                auto members = p[3].split(',');
                
                QString with_whom;
                for (auto &member : members) {
                    if (member != myUsername) {
                        with_whom = member;
                        break;
                    }
                } 

                QString disp = isg
                    ? QString("👥: %1").arg(name)
                    : QString("👤: %1").arg(with_whom);
                auto *it = new QListWidgetItem(disp);
                it->setData(Qt::UserRole, cid);
                it->setData(Qt::UserRole+1, name);
                it->setData(Qt::UserRole+2, members);
                chatsList->addItem(it);
            }
            if (!chunks.isEmpty()) onChatSelected();
            continue;
        }

        // 7) История
        // в onSocketReadyRead(), при обработке HISTORY
        if (line.startsWith("HISTORY ")) {
            chatView->clear();
            QVector<Message> msgs;
            auto chunks = line.mid(8).split(";", Qt::SkipEmptyParts);
            for (const QString &chunk : chunks) {
                QRegularExpression re(R"(\[([^\]]+)\]\s+([^:]+):\s+(.+)\s+\(id=(\d+)\))");
                auto m = re.match(chunk);
                if (!m.hasMatch()) continue;
                Message msg;
                msg.date   = m.captured(1);
                msg.author = m.captured(2);
                msg.text   = m.captured(3);
                msg.id     = m.captured(4).toInt();
                msgs.append(msg);
                chatView->append(
                    QString("[%1] %2: %3")
                        .arg(msg.date, msg.author, msg.text)
                );
            }
            // вот тут сохраняем в кэш
            cache[currentChatId] = msgs;
            continue;
        }


        // 8) Глобальное удаление
        if (line.startsWith("MSG_DELETED ")) {
            bool ok; int mid = line.mid(QString("MSG_DELETED ").length()).toInt(&ok);
            if (!ok) continue;
            // 1) обновляем кэш
            auto &vec = cache[currentChatId];
            for (int i = 0; i < vec.size(); ++i) {
                if (vec[i].id == mid) { vec.remove(i); break; }
            }
            // 2) перерисовываем окно
            chatView->clear();
            for (const Message &m : vec) {
                chatView->append(QString("[%1] %2: %3")
                                .arg(m.date, m.author, m.text));
            }
            continue;
        }

        // 9) Ошибки
        if (line.startsWith("ERROR")) {
            QMessageBox::warning(this,"Error", line);
            continue;
        }
    }
}