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
#include <QNetworkProxy>

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

    //Настраиваем socket
    socket = new QSslSocket(this);
    socket->setPeerVerifyMode(QSslSocket::AutoVerifyPeer);

    connect(socket,
            static_cast<void(QSslSocket::*)(const QList<QSslError>&)>(&QSslSocket::sslErrors),
            this,
            [this](const QList<QSslError> &errs){
                socket->ignoreSslErrors();  //чтобы рукопожатие всё-таки продолжилось
            });

    // 1) При сетевой ошибке (не удалось подключиться, таймаут и т.п.)
    connect(socket, &QAbstractSocket::errorOccurred, this,
        [this](QAbstractSocket::SocketError err) {
            // если мы ещё не показали окно
            static bool shown = false;
            if (!shown) {
                shown = true;
                QMessageBox::critical(
                    this,
                    tr("Ошибка подключения"),
                    socket->errorString()
                );
                qApp->quit();
            }
        }
    );

    // 2) При разрыве соединения после того, как оно было установлено
    connect(socket, &QAbstractSocket::disconnected, this, [this]() {
        QMessageBox::warning(
            this,
            tr("Соединение потеряно"),
            tr("Связь с сервером была разорвана.")
        );
        qApp->quit();
    });

    //Логгируем успешный конец TLS‑рукопожатия
    connect(socket, &QSslSocket::encrypted, this, [](){
        qDebug() << "TLS рукопожатие завершено!";
    });

    connect(socket, &QSslSocket::readyRead, this, &MainWindow::onSocketReadyRead);

    socket->setProxy(QNetworkProxy::NoProxy);

    socket->connectToHostEncrypted(host, port);

    // Стек страниц
    stack = new QStackedWidget(this);
    setCentralWidget(stack);

    // ------- Login Page -------
    pageLogin = new QWidget(this);
    {
        auto *lay = new QVBoxLayout(pageLogin);

        // 1) Растяжка сверху — «пушит» контент вниз
        lay->addStretch(1);

        // 2) Заголовок
        auto *title = new QLabel("<h3>Вход / Регистрация</h3>", pageLogin);
        title->setAlignment(Qt::AlignHCenter);
        lay->addWidget(title);

        lay->addStretch(1);

        // 3) Поля ввода
        usernameEdit = new QLineEdit(pageLogin);
        usernameEdit->setPlaceholderText("Имя пользователя");
        lay->addWidget(usernameEdit);

        passwordEdit = new QLineEdit(pageLogin);
        passwordEdit->setEchoMode(QLineEdit::Password);
        passwordEdit->setPlaceholderText("Пароль");
        lay->addWidget(passwordEdit);

        // 4) Кнопки по центру
        auto *h = new QHBoxLayout();
        loginButton    = new QPushButton("Войти",         pageLogin);
        registerButton = new QPushButton("Зарегистрироваться", pageLogin);
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
        newChatButton  = new QPushButton("Новый личный чат",  pageChats);
        newGroupButton = new QPushButton("Новая группа", pageChats);
        // Верхняя панель: логика чатов + информация о пользователе
        auto *topBar = new QHBoxLayout();
           // слева — кнопки
            topBar->addWidget(newChatButton);
            topBar->addWidget(newGroupButton);
            topBar->addStretch(1);
            // справа — имя пользователя + кнопка «Выйти»

            auto *usernameAndExit = new QVBoxLayout();
            userLabel = new QLabel(this);
            userLabel->setText("");   // пока пусто
            auto *logoutBtn = new QPushButton("Выйти из профиля", this);
            usernameAndExit->addWidget(userLabel);
            usernameAndExit->addWidget(logoutBtn);
            topBar->addLayout(usernameAndExit);

            connect(logoutBtn, &QPushButton::clicked, this, [this]() {
                auto res = QMessageBox::question(this,
                    "Подтвердите выход",
                    "Вы уверены, что хотите выйти из профиля?",
                    QMessageBox::Yes|QMessageBox::No);
                if (res == QMessageBox::Yes) onLogout();
            });
        

        v->addLayout(topBar);

        connect(newChatButton,  &QPushButton::clicked, this, [this](){
            bool ok;
            QString name = QInputDialog::getText(this,"Новый чат",
                              "Введите имя пользователя:", QLineEdit::Normal, {}, &ok);
            if (!ok || name.isEmpty()) return;
            pendingPeerName = name;
            expectingUserId = true;
            sendCmd("GET_USER_ID " + name);
        });
        connect(newGroupButton, &QPushButton::clicked, this, [this](){
            bool ok;
            pendingGroupName = QInputDialog::getText(
                this, "Новая группа", "Название группы:", QLineEdit::Normal, {}, &ok);
            if (!ok || pendingGroupName.isEmpty()) return;
            pendingGroupName.replace(" ", "");
            QString members = QInputDialog::getText(
                this, "Новая группа", "Введите имена пользователей (через пробел):",
                QLineEdit::Normal, {}, &ok);
            if (!ok) return;
            pendingGroupNames = members.split(' ', Qt::SkipEmptyParts);
            pendingGroupIds.clear();
            creatingGroup = true;
            expectingUserId = true;
            sendCmd("GET_USER_ID " + pendingGroupNames.first());
        });

        auto *h = new QHBoxLayout();
        chatsList = new QListWidget(pageChats);
        chatsList->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(chatsList, &QListWidget::customContextMenuRequested,
                this, &MainWindow::onChatsListContextMenu);
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
        sendButton  = new QPushButton("Отправить", pageChats);
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

void MainWindow::appendHtmlLine(const QString &html) {
    auto cursor = chatView->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertHtml(html);
    cursor.insertBlock();  // переходим на следующую строку
    chatView->setTextCursor(cursor);
}

void MainWindow::redrawChatFromCache() {
    chatView->clear();
    for (const ChatEntry &e : cache[currentChatId]) {
        if (e.type == ChatEntry::Message) {
            appendHtmlLine(
                QString("<span style='font-size:small;color:#666;'>[%1]</span> "
                        "<b>%2:</b> %3")
                    .arg(e.date,
                         e.author.toHtmlEscaped(),
                         e.text.toHtmlEscaped())
            );
        } else {
            appendHtmlLine(
                QString("<span style='font-size:small;color:#666;'>[%1]</span> "
                        "<i style='color:rgba(0,0,0,0.6);'>%2</i>")
                    .arg(e.date, e.text.toHtmlEscaped())
            );
        }
    }
}

void MainWindow::onChatsListContextMenu(const QPoint &pt) {
    auto *item = chatsList->itemAt(pt);
    if (!item) return;

    bool isGroup = item->data(Qt::UserRole+3).toBool(); // нужно сохранить признак is_group
    QMenu menu;
    QAction *leaveAct = nullptr;
    if (isGroup) {
        leaveAct = menu.addAction("Покинуть группу");
    }
    QAction *act = menu.exec(chatsList->mapToGlobal(pt));
    if (act == leaveAct && isGroup) {
        if (QMessageBox::question(this, "Покинуть группу",
              "Вы действительно хотите покинуть эту группу?",
              QMessageBox::Yes|QMessageBox::No) == QMessageBox::Yes)
        {
            int cid = item->data(Qt::UserRole).toInt();
            sendCmd(QString("LEAVE_CHAT %1").arg(cid));
            chatView->clear();
            // локально удаляем из списка:
            delete item;
        }
    }
}

void MainWindow::onChatViewContextMenu(const QPoint &pt) {
    QTextCursor cursor = chatView->cursorForPosition(pt);
    QTextBlock  block  = cursor.block();                    // теперь работает
    int blockNo = block.blockNumber();

    // дальше вы можете по номеру блока смотреть cache[currentChatId][blockNo].id
    // и удалять именно то сообщение
    QMenu menu;
    QAction *delMe  = menu.addAction("Удалить для себя");
    QAction *delAll = menu.addAction("Удалить для всех");
    QAction *act = menu.exec(QCursor::pos());
    if (!act) return;
    int msgId = cache[currentChatId][blockNo].id;

    if (act == delMe) {
        auto ok = QMessageBox::question(this, "Удалить сообщение",
            "Вы уверены, что хотите удалить это сообщение у себя?",
            QMessageBox::Yes|QMessageBox::No);
        if (ok == QMessageBox::Yes)
            sendCmd(QString("DELETE %1").arg(msgId));

    } else if (act == delAll) {
        auto ok = QMessageBox::question(this, "Удалить сообщение у всех",
            "Вы уверены, что хотите удалить это сообщение у всех участников?",
            QMessageBox::Yes|QMessageBox::No);
        if (ok == QMessageBox::Yes)
            sendCmd(QString("DELETE_GLOBAL %1").arg(msgId));
    }
}

// Слот: зарегистрироваться
void MainWindow::onRegister() {
    QString u = usernameEdit->text(), p = passwordEdit->text();
    if (u.isEmpty() || p.isEmpty()) {
        QMessageBox::warning(this,"Ошибка","Введите имя пользователя и пароль");
        return;
    }
    sendCmd(QString("REGISTER %1 %2").arg(u,p));
}

// Слот: залогиниться
void MainWindow::onLogin() {
    QString u = usernameEdit->text(), p = passwordEdit->text();
    if (u.isEmpty() || p.isEmpty()) {
        QMessageBox::warning(this,"Ошибка","Введите имя пользователя и пароль");
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

    // Если есть кэш — рисуем из него
    if (cache.contains(currentChatId)) {
        redrawChatFromCache();
    }

    // Иначе — запрашиваем у сервера
    sendCmd(QString("HISTORY %1").arg(currentChatId));
}

void MainWindow::onSend() {
    if (currentChatId < 0) return;
    QString msg = messageEdit->text().trimmed();
    if (msg.isEmpty()) return;

    // 1) формируем текущую временную метку
    QString now = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm");

    // 2) Подготавляем ChatEntry (ид ещё неизвестно, запишем -1)
    ChatEntry e;
    e.type   = ChatEntry::Message;
    e.date   = now;
    e.author = myUsername;
    e.text   = msg;
    e.id     = -1;

    // 3) кладём в кэш
    cache[currentChatId].append(e);

    // 4) сразу выводим в chatView тем же HTML‑шаблоном
    appendHtmlLine(
        QString("<span style='font-size:small;color:#666;'>[%1]</span> "
                "<b>%2:</b> %3")
            .arg(e.date,
                 e.author.toHtmlEscaped(),
                 e.text.toHtmlEscaped())
    );

    // 5) отправляем на сервер
    sendCmd(QString("SEND %1 ").arg(currentChatId) + msg);

    // 6) чистим поле ввода
    messageEdit->clear();

    // 7) сохраняем в lastMessage на случай OK_SENT
    lastMessage = e;
}

// Слот: пришли данные от сервера
void MainWindow::onSocketReadyRead() {
    QByteArray data = socket->readAll();
    QStringList lines = QString::fromUtf8(data)
                          .split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        qDebug() << "[recievedFromServer]:" << line;

        //1) Ответ на GET_USER_ID (для создания чата)
        if (expectingUserId) {
            expectingUserId = false;
            bool ok; int uid = line.toInt(&ok);
            if (!ok || uid<=0) {
                QMessageBox::warning(this,"Ошибка",
                    "Пользователь \"" + pendingPeerName + "\" не найден!");
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
            QMessageBox::warning(this,"Ошибка",
                                     "Личный чат с данным пользователем уже существует");
            continue;
        }

        // 3) Успешный логин
        if (line.startsWith("OK LOGIN")) {
            myUserId = line.split(' ')[1].toInt();
            userLabel->setText(QString("Пользователь: %1").arg(myUsername));
            stack->setCurrentWidget(pageChats);

            sendCmd("LIST_CHATS");
            continue;
        }

        if (line.startsWith("OK REG")) {
            QMessageBox::information(this, "Успешная регистрация!", "Теперь заходим в аккаунт...");
            onLogin();
            continue;
        }

        if (line.startsWith("OK SENT")) {
            bool ok;
            int mid = line.mid(QString("OK SENT ").length()).toInt(&ok);
            if (!ok) return;

            // Проставляем настоящий id — ищем последний записанный элемент с id=-1
            auto &vec = cache[currentChatId];
            for (int i = vec.size()-1; i >= 0; --i) {
                if (vec[i].id == -1 && vec[i].author == myUsername) {
                    vec[i].id = mid;
                    break;
                }
            }
            // у нас уже всё отображено, поэтому перерисовывать не нужно
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
                it->setData(Qt::UserRole+3, isg);
                chatsList->addItem(it);
            }
            if (!chunks.isEmpty()) onChatSelected();
            continue;
        }

        // 7) История
        if (line.startsWith("HISTORY ")) {
            chatView->clear();
            QVector<ChatEntry> entries;
            auto chunks = line.mid(8).split(";", Qt::SkipEmptyParts);

            QRegularExpression reMsg(R"(\[([^\]]+)\]\s+([^:]+):\s+(.+)\s+\(id=(\d+)\))");
            QRegularExpression reEvt(R"(\[([^\]]+)\]\s+\*\s+(.+))");

            for (const QString &chunk : chunks) {
                if (auto m = reMsg.match(chunk); m.hasMatch()) {
                    ChatEntry e;
                    e.type   = ChatEntry::Message;
                    e.date   = m.captured(1);
                    e.author = m.captured(2);
                    e.text   = m.captured(3);
                    e.id     = m.captured(4).toInt();
                    entries.append(e);

                    appendHtmlLine(
                        QString("<span style='font-size:small;color:#666;'>[%1]</span> "
                                "<b>%2:</b> %3")
                            .arg(e.date,
                                e.author.toHtmlEscaped(),
                                e.text.toHtmlEscaped())
                    );
                }
                else if (auto m = reEvt.match(chunk); m.hasMatch()) {
                    ChatEntry e;
                    e.type = ChatEntry::Event;
                    e.date = m.captured(1);
                    e.text = m.captured(2);
                    entries.append(e);

                    appendHtmlLine(
                        QString("<span style='font-size:small;color:#666;'>[%1]</span> "
                                "<i style='color:rgba(0,0,0,0.6);'>*** %2 ***</i>")
                            .arg(e.date, e.text.toHtmlEscaped())
                    );
                }
            }

            cache[currentChatId] = std::move(entries);
            continue;
        }

        if (line.startsWith("USER_LEFT")) {
            // USER_LEFT <chat_id> <username> <HH:MM>
            auto parts = line.split(' ');
            int cid     = parts[1].toInt();
            QString who = parts[2];
            QString ts  = parts[3] + " " + parts[4];

            ChatEntry e;
            e.type = ChatEntry::Event;
            e.date = ts;
            e.text = QString("%1 покинул(а) чат").arg(who);

            cache[cid].append(e);
            if (cid == currentChatId) {
                appendHtmlLine(
                    QString("<span style='font-size:small;color:#666;'>[%1]</span> "
                            "<i style='color:rgba(0,0,0,0.6);'>*** %2 ***</i>")
                        .arg(e.date, e.text.toHtmlEscaped())
                );
            }
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
            redrawChatFromCache();
            continue;
        }

        if (line.startsWith("ERROR USER_EXISTS")) {
            QMessageBox::warning(this,"Ошибка", "Такой пользователь уже существует!");
            continue;
        }

        if (line.startsWith("ERROR NOT_CORRECT")) {
            QMessageBox::warning(this,"Ошибка", "Неверное имя пользователя или пароль!");
            continue;
        }

        if (line.startsWith("ERROR NO_RIGHTS")) {
            QMessageBox::warning(this,"Ошибка", "Вы можете удалять только собственные сообщения!");
            continue;
        }

        // 9) Ошибки
        if (line.startsWith("ERROR")) {
            QMessageBox::warning(this,"Ошибка", line);
            continue;
        }
    }
}