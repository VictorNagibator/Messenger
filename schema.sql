-- Создание таблицы пользователей
CREATE TABLE users (
  user_id SERIAL PRIMARY KEY,
  username VARCHAR(50) UNIQUE NOT NULL,
  password_hash TEXT NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Таблица чатов
CREATE TABLE chats (
  chat_id SERIAL PRIMARY KEY,
  chat_name VARCHAR(100),          -- имя группы (NULL для приватного чата)
  is_group BOOLEAN NOT NULL DEFAULT FALSE,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Участники чатов
CREATE TABLE chat_members (
  chat_id INT NOT NULL
    REFERENCES chats(chat_id) ON DELETE CASCADE,
  user_id INT NOT NULL
    REFERENCES users(user_id) ON DELETE CASCADE,
  PRIMARY KEY(chat_id, user_id)
);

-- Сообщения
CREATE TABLE messages (
  msg_id SERIAL PRIMARY KEY,
  chat_id INT NOT NULL
    REFERENCES chats(chat_id) ON DELETE CASCADE,
  sender_id INT NOT NULL
    REFERENCES users(user_id) ON DELETE CASCADE,
  content TEXT NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  deleted BOOLEAN NOT NULL DEFAULT FALSE
);

-- Таблица для пометки «удалил для себя»
CREATE TABLE user_deleted_messages (
  user_id INT NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
  msg_id  INT NOT NULL REFERENCES messages(msg_id) ON DELETE CASCADE,
  PRIMARY KEY(user_id, msg_id)
);

-- Индексы
CREATE INDEX idx_messages_chat     ON messages(chat_id);
CREATE INDEX idx_messages_sender   ON messages(sender_id);
CREATE INDEX idx_chat_members_user ON chat_members(user_id);