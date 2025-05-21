QT       += core gui widgets network

CONFIG   += c++17

SOURCES  += src/main.cpp \
            src/mainwindow.cpp

HEADERS  += src/mainwindow.h

# Для OpenSSL
LIBS    += -lssl -lcrypto