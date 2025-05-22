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
    //По умолчанию подключаемся к локальному серверу на порту 12345
    QString host = "127.0.0.1";
    int port = 12345;

    //Путь до файла с настройками подключения (должен быть в одной папке с исполняемым файлом)
    QString cfgPath = "./config.ini";

    //Если файл конфигурации существует — читаем из него host и port
    //(Хотел изначально через QSettings, но там никак не хотело корректно брать настройки :( )
    QFile f(cfgPath);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.startsWith("host=")) {
                //"host=..." оставляем всё после "host="
                host = line.mid(sizeof("host=") - 1).trimmed();
            }
            else if (line.startsWith("port=")) {
                // "port=..." конвертируем строку в число
                port = line.mid(sizeof("port=") - 1).toInt();
            }
        }
    }

    //Настраиваем защищённый SSL-сокет для связи с сервером
    socket = new QSslSocket(this);
    socket->setPeerVerifyMode(QSslSocket::AutoVerifyPeer); //QSslSocket автоматически запрашивает сертификаты

    //В случае ошибок SSL-сертификата (самоподписанный, как в нашем случае)
    //игнорируем их, чтобы рукопожатие всё же завершилось
    connect(socket,
            static_cast<void(QSslSocket::*)(const QList<QSslError>&)>(&QSslSocket::sslErrors),
            this,
            [this](const QList<QSslError> &){
                socket->ignoreSslErrors();
            });


    //Обрабатываем сетевые ошибки (не удалось подключиться, таймаут и т.п.)
    connect(socket, &QAbstractSocket::errorOccurred, this,
        [this](QAbstractSocket::SocketError /*err*/) {
            static bool shown = false;
            if (!shown) {
                shown = true;
                //Показываем  окно с текстом ошибки
                QMessageBox::critical(
                    this,
                    tr("Ошибка подключения"),
                    socket->errorString()
                );
                //Завершаем приложение
                qApp->quit();
            }
        }
    );

    //Когда на сокете появляются данные — передаём их в onSocketReadyRead()
    connect(socket, &QSslSocket::readyRead, this, &MainWindow::onSocketReadyRead);

    //Отключаем использование системного прокси (на Windows строго обязательно, без этого ни на одной станции не запустилось!)
    socket->setProxy(QNetworkProxy::NoProxy);

    //Запускаем зашифрованное подключение к серверу.
    socket->connectToHostEncrypted(host, port);

    //UI: создаём стек страниц (login и chats)
    stack = new QStackedWidget(this);
    setCentralWidget(stack);

    //СТАРТОВАЯ СТРАНИЦА: ВХОД / РЕГИСТРАЦИЯ
    pageLogin = new QWidget(this);
    {
        auto *lay = new QVBoxLayout(pageLogin);

        //1) Верхняя пустая растяжка, чтобы контент был по центру
        lay->addStretch(1);

        //2) Заголовок страницы
        auto *title = new QLabel("<h3>Вход / Регистрация</h3>", pageLogin);
        title->setAlignment(Qt::AlignHCenter);
        lay->addWidget(title);

        lay->addStretch(1);

        //3) Поля для ввода имени и пароля
        usernameEdit = new QLineEdit(pageLogin);
        usernameEdit->setPlaceholderText("Имя пользователя");
        lay->addWidget(usernameEdit);

        passwordEdit = new QLineEdit(pageLogin);
        passwordEdit->setEchoMode(QLineEdit::Password);
        passwordEdit->setPlaceholderText("Пароль");
        lay->addWidget(passwordEdit);

        //4) Кнопки «Войти» и «Зарегистрироваться» по центру
        auto *h = new QHBoxLayout();
        loginButton  = new QPushButton("Войти", pageLogin);
        registerButton = new QPushButton("Зарегистрироваться", pageLogin);
        h->addWidget(loginButton);
        h->addWidget(registerButton);
        lay->addLayout(h);

        // одключаем слоты нажатия кнопок
        connect(loginButton, &QPushButton::clicked, this, &MainWindow::onLogin);
        connect(registerButton, &QPushButton::clicked, this, &MainWindow::onRegister);
    }
    stack->addWidget(pageLogin);

    //СТРАНИЦА ЧАТОВ
    pageChats = new QWidget(this);
    {
        auto *v = new QVBoxLayout(pageChats);

        //Верхняя панель с кнопками «Новый чат», «Новая группа» и «Выйти»
        newChatButton  = new QPushButton("Новый личный чат", pageChats);
        newGroupButton = new QPushButton("Новая группа", pageChats);
        auto *topBar = new QHBoxLayout();
        topBar->addWidget(newChatButton);
        topBar->addWidget(newGroupButton);
        topBar->addStretch(1);

        //Справа показываем имя пользователя и кнопку «Выйти»
        auto *usernameAndExit = new QVBoxLayout();
        userLabel = new QLabel(this);
        userLabel->setText(""); // пока пусто
        auto *logoutBtn = new QPushButton("Выйти из профиля", this);
        usernameAndExit->addWidget(userLabel);
        usernameAndExit->addWidget(logoutBtn);
        topBar->addLayout(usernameAndExit);

        //Нажатие «Выйти» → подтверждение и вызов onLogout()
        connect(logoutBtn, &QPushButton::clicked, this, [this]() {
            auto res = QMessageBox::question(
                this,
                "Подтвердите выход",
                "Вы уверены, что хотите выйти из профиля?",
                QMessageBox::Yes | QMessageBox::No
            );
            if (res == QMessageBox::Yes)
                onLogout();
        });

        v->addLayout(topBar);

        //Обработчики кнопок создания нового чата/группы
        connect(newChatButton,  &QPushButton::clicked, this, [this](){
            bool ok;
            QString name = QInputDialog::getText(
                this, "Новый чат",
                "Введите имя пользователя:",
                QLineEdit::Normal, {}, &ok
            );
            if (!ok || name.isEmpty())
                return;
            pendingPeerName = name;
            expectingUserId = true;
            sendCmd("GET_USER_ID " + name);
        });
        connect(newGroupButton, &QPushButton::clicked, this, [this](){
            bool ok;
            pendingGroupName = QInputDialog::getText(
                this, "Новая группа",
                "Название группы:",
                QLineEdit::Normal, {}, &ok
            );
            if (!ok || pendingGroupName.isEmpty())
                return;
            pendingGroupName.replace(" ", "_");  //удаляем пробелы из названия
            QString members = QInputDialog::getText(
                this, "Новая группа",
                "Введите имена пользователей (через пробел):",
                QLineEdit::Normal, {}, &ok
            );
            if (!ok)
                return;
            //Сохраняем список юзернеймов для получения их ID
            pendingGroupNames = members.split(' ', Qt::SkipEmptyParts);
            pendingGroupIds.clear();
            creatingGroup = true;
            expectingUserId = true;

            //Запрашиваем ID первого пользователя
            sendCmd("GET_USER_ID " + pendingGroupNames.first());
        });

        //Основная область: список чатов + окно сообщений + ввод сообщения
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
                this, &MainWindow::onChatViewContextMenu);
        r->addWidget(chatView, 1);

        auto *sh = new QHBoxLayout();
        messageEdit = new QLineEdit(pageChats);
        sendButton  = new QPushButton("Отправить", pageChats);
        sh->addWidget(messageEdit, 1);
        sh->addWidget(sendButton);
        r->addLayout(sh);

        h->addLayout(r, 2);
        v->addLayout(h);

        //Клики по списку чат - onChatSelected()
        connect(chatsList, &QListWidget::itemClicked, this, &MainWindow::onChatSelected);
        //Нажатие «Отправить» - onSend()
        connect(sendButton, &QPushButton::clicked, this, &MainWindow::onSend);
    }
    stack->addWidget(pageChats);

    //По умолчанию показываем страницу логина
    stack->setCurrentWidget(pageLogin);
}

//Отправка произвольной команды на сервер по SSL-сокету
//cmd: строка команды без завершающего '\n'
void MainWindow::sendCmd(const QString &cmd) {
    //Добавляем символ новой строки и отправляем байты
    socket->write(cmd.toUtf8() + "\n"); //UTF-8 !!!
}

//Вставляет HTML‑строку в конец окна чата и переходит на новую строку (история так выводится)
void MainWindow::appendHtmlLine(const QString &html) {
    //Получаем текущий курсор текста
    auto cursor = chatView->textCursor();
    //Перемещаем курсор в конец документа
    cursor.movePosition(QTextCursor::End);
    //Вставляем HTML (сохранится форматирование)
    cursor.insertHtml(html);
    //Вставляем разрыв строки (новый параграф)
    cursor.insertBlock();
    //Применяем курсор обратно к QTextEdit
    chatView->setTextCursor(cursor);
}

//Полностью перерисовывает историю сообщений из локального кэша
void MainWindow::redrawChatFromCache() {
    //Очищаем текущее содержимое виджета
    chatView->clear();

    //Проходим по всем записям текущего чата
    for (const ChatEntry &e : cache[currentChatId]) {
        if (e.type == ChatEntry::Message) {
            //Для обычных сообщений показываем дату, автора и текст
            appendHtmlLine(
                QString("<span style='font-size:small;color:#666;'>[%1]</span> "
                        "<b>%2:</b> %3")
                    .arg(e.date, //временная метка
                         e.author.toHtmlEscaped(), //имя автора
                         e.text.toHtmlEscaped()) //текст сообщения
            );
        } else {
            //Для системных событий показываем дату и курсивом текст
            appendHtmlLine(
                QString("<span style='font-size:small;color:#666;'>[%1]</span> "
                        "<i style='color:rgba(0,0,0,0.6);'>%2</i>")
                    .arg(e.date, //временная метка
                         e.text.toHtmlEscaped()) //текст события
            );
        }
    }
}

//Контекстное меню для списка чатов: позволяет «покинуть группу»
void MainWindow::onChatsListContextMenu(const QPoint &pt) {
    //Определяем элемент под курсором
    auto *item = chatsList->itemAt(pt);
    if (!item) return;

    //Проверяем, является ли этот чат группой (сохранили признак в UserRole+3)
    bool isGroup = item->data(Qt::UserRole + 3).toBool();

    QMenu menu;
    QAction *leaveAct = nullptr;

    //Если это группа — добавляем пункт «Покинуть группу»
    if (isGroup) {
        leaveAct = menu.addAction("Покинуть группу");
    }

    //Показываем меню в координатах экрана
    QAction *act = menu.exec(chatsList->mapToGlobal(pt));
    //Если пользователь выбрал «Покинуть группу» — отправляем команду
    if (act == leaveAct && isGroup) {
        if (QMessageBox::question(this,
                "Покинуть группу",
                "Вы действительно хотите покинуть эту группу?",
                QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
        {
            int cid = item->data(Qt::UserRole).toInt(); //chat_id
            sendCmd(QString("LEAVE_CHAT %1").arg(cid)); //отправка на сервер
            chatView->clear(); //очищаем окно сообщений
            delete item; //убираем из списка чатов
        }
    }
}

//Контекстное меню в окне сообщений: удаление «для себя» или «для всех»
void MainWindow::onChatViewContextMenu(const QPoint &pt) {
    //Находим блок текста, по которому кликнули
    QTextCursor cursor = chatView->cursorForPosition(pt);
    QTextBlock  block  = cursor.block();
    int blockNo = block.blockNumber(); //порядковый номер блока

    //Создаём меню и два варианта действий
    QMenu menu;
    QAction *delMe  = menu.addAction("Удалить для себя");
    QAction *delAll = menu.addAction("Удалить для всех");

    //Показываем меню возле курсора
    QAction *act = menu.exec(QCursor::pos());
    if (!act) return;

    //Получаем msg_id из кэша по номеру блока
    int msgId = cache[currentChatId][blockNo].id;

    if (act == delMe) {
        //Подтверждение удаления для себя
        if (QMessageBox::question(this,
                "Удалить сообщение",
                "Вы уверены, что хотите удалить это сообщение у себя?",
                QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
        {
            sendCmd(QString("DELETE %1").arg(msgId));
        }
    }
    else if (act == delAll) {
        //Подтверждение удаления для всех
        if (QMessageBox::question(this,
                "Удалить сообщение у всех",
                "Вы уверены, что хотите удалить это сообщение у всех участников?",
                QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
        {
            sendCmd(QString("DELETE_GLOBAL %1").arg(msgId));
        }
    }
}

//Слот для регистрации нового пользователя
void MainWindow::onRegister() {
    QString u = usernameEdit->text();
    QString p = passwordEdit->text();

    //Проверяем, что поля не пустые
    if (u.isEmpty() || p.isEmpty()) {
        QMessageBox::warning(this, "Ошибка", "Введите имя пользователя и пароль");
        return;
    }

    //Формируем команду и отправляем на сервер
    sendCmd(QString("REGISTER %1 %2").arg(u, p));
}

//Слот для входа в существующий аккаунт
void MainWindow::onLogin() {
    QString u = usernameEdit->text();
    QString p = passwordEdit->text();
    //Проверяем заполненность полей
    if (u.isEmpty() || p.isEmpty()) {
        QMessageBox::warning(this, "Ошибка", "Введите имя пользователя и пароль");
        return;
    }
    //Сохраняем локально имя для отображения
    myUsername = u;
    //Отправляем команду входа
    sendCmd(QString("LOGIN %1 %2").arg(u, p));
}

//Слот для выхода из аккаунта
void MainWindow::onLogout() {
    //Сбрасываем идентификаторы
    myUserId  = -1;
    currentChatId = -1;

    //Очищаем список чатов и окно сообщений
    chatsList->clear();
    chatView->clear();

    // брасываем поля ввода
    usernameEdit->clear();
    passwordEdit->clear();

    //Очищаем локальный кеш
    cache.clear();

    //Показываем страницу логина
    stack->setCurrentWidget(pageLogin);
}

//Слот: пользователь выбрал чат из списка
void MainWindow::onChatSelected() {
    auto *it = chatsList->currentItem();
    if (!it) return;

    //Получаем chat_id из данных элемента
    currentChatId = it->data(Qt::UserRole).toInt();

    //Если история чата уже закэширована — рисуем её
    if (cache.contains(currentChatId)) {
        redrawChatFromCache();
    }
    //Иначе запрашиваем историю у сервера
    else {
        sendCmd(QString("HISTORY %1").arg(currentChatId));
    }
}

//Слот: нажатие кнопки «Отправить»
void MainWindow::onSend() {
    //Проверяем, что чат выбран
    if (currentChatId < 0) return;

    //Читаем и обрезаем сообщение
    QString msg = messageEdit->text().trimmed();
    if (msg.isEmpty()) return;

    //1) Получаем текущую временную метку
    QString now = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm");

    //2) Подготавливаем локальную запись ChatEntry (id ещё неизвестен)
    ChatEntry e;
    e.type = ChatEntry::Message;
    e.date = now;
    e.author = myUsername;
    e.text = msg;
    e.id  = -1;

    //3) Добавляем в кэш
    cache[currentChatId].append(e);

    //4) Мгновенно отображаем в окне чата
    appendHtmlLine(
        QString("<span style='font-size:small;color:#666;'>[%1]</span> "
                "<b>%2:</b> %3")
            .arg(e.date,
                 e.author.toHtmlEscaped(),
                 e.text.toHtmlEscaped())
    );

    //5) Отправляем сообщение серверу
    sendCmd(QString("SEND %1 ").arg(currentChatId) + msg);

    //6) Очищаем поле ввода
    messageEdit->clear();
}

//Слот: пришли данные от сервера
void MainWindow::onSocketReadyRead() {
    //Читаем все доступные данные из SSL‑сокета
    QByteArray data = socket->readAll();
    //Разбиваем на строки по символу '\n', игнорируем пустые
    QStringList lines = QString::fromUtf8(data)
                          .split('\n', Qt::SkipEmptyParts);

    //Обрабатываем каждую строку отдельно
    for (const QString &line : lines) {
        //1) Ответ на GET_USER_ID — следующий ответ мы ожидаем после запроса GET_USER_ID
        if (line.startsWith("USER_ID")) {
            //Сбрасываем флаг ожидания ответа
            expectingUserId = false;

            //Пытаемся преобразовать строку в число user_id
            int uid = line.split(' ')[1].toInt();
            if (uid <= 0) {
                //Если не удалось — показываем предупреждение
                QMessageBox::warning(
                    this,
                    "Ошибка",
                    "Пользователь \"" + pendingPeerName + "\" не найден!"
                );
            } else {
                //Если успешно нашли ID
                if (creatingGroup) {
                    //(мы в процессе создания группы)
                    //сохраняем новый user_id
                    pendingGroupIds.append(uid);

                    if (pendingGroupIds.size() < pendingGroupNames.size()) {
                        //Если ещё остались имена для поиска,
                        //продолжаем запрашивать ID следующего участника
                        expectingUserId = true;
                        sendCmd("GET_USER_ID " + pendingGroupNames[pendingGroupIds.size()]);
                    } else {
                        //Все ID участников получены — отправляем CREATE_CHAT для группы
                        QString cmd = "CREATE_CHAT 1 " + pendingGroupName;
                        for (int x : pendingGroupIds) {
                            cmd += " " + QString::number(x);
                        }
                        sendCmd(cmd);
                        creatingGroup = false;
                    }
                } else {
                    //Обычный приватный чат — отправляем CREATE_CHAT с флагом 0 и peer ID
                    sendCmd(QString("CREATE_CHAT 0 %1").arg(uid));
                }
            }
            //После обработки GET_USER_ID переходим к следующей строке
            continue;
        }

        //2) Обработка ошибок создания приватного чата
        if (line.startsWith("ERROR CHAT_EXISTS")) {
            QMessageBox::warning(
                this, 
                "Ошибка",
                "Личный чат с данным пользователем уже существует"
            );
            continue;
        }

        //3) Успешный логин — сервер вернул "OK LOGIN <user_id>"
        if (line.startsWith("OK LOGIN")) {
            //Извлекаем наш user_id
            myUserId = line.split(' ')[1].toInt();
            //Обновляем лейбл в UI
            userLabel->setText(QString("Пользователь: %1").arg(myUsername));
            //Показываем страницу со списком чатов
            stack->setCurrentWidget(pageChats);
            //Запрашиваем список чатов
            sendCmd("LIST_CHATS");
            continue;
        }

        //4) Успешная регистрация — сервер вернул "OK REG"
        if (line.startsWith("OK REG")) {
            QMessageBox::information(
                this,
                "Успешная регистрация!",
                "Теперь заходим в аккаунт..."
            );
            //Автоматически вызываем onLogin для входа
            onLogin();
            continue;
        }

        //5) Подтверждение отправки сообщения — "OK SENT <msg_id>"
        if (line.startsWith("OK SENT")) {
            bool ok;
            int mid = line.mid(QString("OK SENT ").length()).toInt(&ok);
            if (!ok) {
                //Не смогли распознать msg_id — выходим
                return;
            }

            //Находим в кэше последний наш локальный ChatEntry с id = -1
            auto &vec = cache[currentChatId];
            for (int i = vec.size() - 1; i >= 0; --i) {
                if (vec[i].id == -1 && vec[i].author == myUsername) {
                    //Присваиваем ему настоящий msg_id
                    vec[i].id = mid;
                    break;
                }
            }

            //UI уже содержит сообщение, перерисовывать не нужно
            continue;
        }

        //6) Уведомление о создании нового чата — "NEW_CHAT"
        if (line.startsWith("NEW_CHAT")) {
            //Формат: NEW_CHAT <cid> <is_group> <name_or_member1, member2>
            QStringList parts = line.split(' ');
            int cid = parts[1].toInt();
            bool isGroup = (parts[2] == "1");
            QString nameOrList = parts[3];
            QString display;
            if (isGroup) {
                display = QString("👥: %1").arg(nameOrList);
            } else {
                //[3] == "viktor,aleksey"
                QStringList m = nameOrList.split(',');
                QString other = (m[0] == myUsername ? m[1] : m[0]);
                display = QString("👤: %1").arg(other);
            }
            auto *item = new QListWidgetItem(display);
            item->setData(Qt::UserRole, cid);
            item->setData(Qt::UserRole+1, nameOrList);
            item->setData(Qt::UserRole+3, isGroup);
            chatsList->addItem(item);
            continue;
        }

        //7) Уведомление о новом сообщении
        if (line.startsWith("NEW_MESSAGE")) {
            //Формат: NEW_MESSAGE <chat_id> <msg_id> <YYYY-MM-DD> <HH:MM> <from> <content>
            //Разобъём по пробелам, но content может содержать пробелы, поэтому делаем так:
            QString payload = line.mid(QString("NEW_MESSAGE ").length());
            QStringList parts = payload.split(' ');
            int cid  = parts[0].toInt();
            int mid  = parts[1].toInt();
            QString date = parts[2] + " " + parts[3];
            QString from = parts[4];
            //Всё остальное — content
            QString content = parts.mid(5).join(' ');

            //Добавляем в кэш
            ChatEntry e;
            e.type   = ChatEntry::Message;
            e.date   = date;
            e.author = from;
            e.text   = content;
            e.id     = mid;
            cache[cid].append(e);

            //Если это текущий открытый чат — выводим прямо сейчас
            if (cid == currentChatId) {
                appendHtmlLine(
                    QString("<span style='font-size:small;color:#666;'>[%1]</span> "
                            "<b>%2:</b> %3")
                        .arg(e.date, e.author.toHtmlEscaped(), e.text.toHtmlEscaped())
                );
            }
            continue;
        }

        //8) Обработка списка чатов — "CHATS <cid>:<is_group>:<name>:<members>;..."
        if (line.startsWith("CHATS")) {
            chatsList->clear();

            //Убираем префикс "CHATS " и разбиваем на куски по ';'
            auto chunks = line.mid(6).split(';', Qt::SkipEmptyParts);
            for (auto &chunk : chunks) {
                //Каждый chunk: "cid:is_group:name:member1,member2,..."
                auto p = chunk.split(':');
                int cid = p[0].toInt();
                bool isGroup = (p[1] == "1");
                QString name = p[2];
                auto members = p[3].split(',');

                //Для личного чата показываем имя «с кем», для группы — саму группу
                QString display;
                if (isGroup) {
                    display = QString("👥: %1").arg(name);
                } else {
                    //Находим имя другого участника
                    for (const QString &m : members) {
                        if (m != myUsername) {
                            display = QString("👤: %1").arg(m);
                            break;
                        }
                    }
                }

                //Создаём элемент списка и сохраняем метаданные (потом удобно знать информацию о чате)
                auto *item = new QListWidgetItem(display);
                item->setData(Qt::UserRole + 0, cid);
                item->setData(Qt::UserRole + 1, name);
                item->setData(Qt::UserRole + 2, members);
                item->setData(Qt::UserRole + 3, isGroup);
                chatsList->addItem(item);
            }

            //Если список не пустой, выбираем первый чат
            if (!chunks.isEmpty()) {
                onChatSelected();
            }
            continue;
        }

        //9) Обработка истории сообщений — "HISTORY <entries>;"
        if (line.startsWith("HISTORY")) {
            chatView->clear();
            QVector<ChatEntry> entries;
            //Отрезаем "HISTORY " и разбиваем по ';'
            auto chunks = line.mid(8).split(";", Qt::SkipEmptyParts);

            //Регэкспы для сообщений и системных событий
            QRegularExpression reMsg(R"(\[([^\]]+)\]\s+([^:]+):\s+(.+)\s+\(id=(\d+)\))");
            QRegularExpression reEvt(R"(\[([^\]]+)\]\s+\*\s+(.+))");

            //Проходим по каждому фрагменту
            for (const QString &chunk : chunks) {
                if (auto m = reMsg.match(chunk); m.hasMatch()) {
                    //Сообщение: [timestamp] author: content (id=msg_id)
                    ChatEntry e;
                    e.type = ChatEntry::Message;
                    e.date = m.captured(1);
                    e.author = m.captured(2);
                    e.text = m.captured(3);
                    e.id = m.captured(4).toInt();
                    entries.append(e);
                }
                else if (auto m = reEvt.match(chunk); m.hasMatch()) {
                    //Событие: [timestamp] * description
                    ChatEntry e;
                    e.type = ChatEntry::Event;
                    e.date = m.captured(1);
                    e.text = m.captured(2);
                    entries.append(e);
                }
            }

            //Сохраняем полную историю в кэш
            cache[currentChatId] = std::move(entries);
            //И перерисуем
            redrawChatFromCache();
            continue;
        }

        //10) Уведомление о выходе пользователя — "USER_LEFT <chat_id> <username> <YYYY-MM-DD> <HH:MM>"
        if (line.startsWith("USER_LEFT")) {
            auto parts = line.split(' ');
            int cid = parts[1].toInt();
            QString who = parts[2];
            QString ts = parts[3] + " " + parts[4];

            ChatEntry e;
            e.type = ChatEntry::Event;
            e.date = ts;
            e.text = QString("%1 покинул(а) чат").arg(who);

            //Добавляем в кэш и, если открыт этот чат — отображаем сразу
            cache[cid].append(e);
            if (cid == currentChatId) {
                redrawChatFromCache();
            }
            continue;
        }

        //11) Глобальное удаление сообщения — "MSG_DELETED <msg_id>"
        if (line.startsWith("MSG_DELETED")) {
            auto parts = line.split(' ');
            int cid = parts[1].toInt();
            int msg_id = parts[2].toInt();

            //Удаляем запись из локального кэша
            auto &vec = cache[cid];
            for (int i = 0; i < vec.size(); ++i) {
                if (vec[i].id == msg_id) {
                    vec.remove(i);
                    break;
                }
            }

            if (cid == currentChatId) {
                //Перерисовываем окно чата
                redrawChatFromCache();
            }

            continue;
        }

        //12) Информационные ошибки от сервера
        if (line == "ERROR USER_EXISTS") {
            QMessageBox::warning(this, "Ошибка", "Такой пользователь уже существует!");
            continue;
        }
        if (line == "ERROR NOT_CORRECT") {
            QMessageBox::warning(this, "Ошибка", "Неверное имя пользователя или пароль!");
            continue;
        }
        if (line == "ERROR NO_RIGHTS") {
            QMessageBox::warning(this, "Ошибка", "Вы можете удалять только собственные сообщения!");
            continue;
        }

        // 13) Любая другая ошибка — показываем текст ошибки
        if (line.startsWith("ERROR")) {
            QMessageBox::warning(this, "Ошибка", line);
            continue;
        }
    }
}