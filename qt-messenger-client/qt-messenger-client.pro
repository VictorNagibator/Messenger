QT       += core gui widgets network

CONFIG   += c++17

SOURCES  += main.cpp \
            mainwindow.cpp \
            crypto.cpp

HEADERS  += mainwindow.h \
            crypto.h

# Для OpenSSL
LIBS    += -lssl -lcrypto