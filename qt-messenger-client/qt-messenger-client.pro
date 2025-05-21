QT       += core gui widgets network

CONFIG   += c++17

SOURCES  += main.cpp \
            mainwindow.cpp

HEADERS  += mainwindow.h

# Для OpenSSL
LIBS    += -lssl -lcrypto