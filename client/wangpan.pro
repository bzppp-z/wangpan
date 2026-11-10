QT += core gui widgets network
CONFIG += c++11
CONFIG -= app_bundle
TARGET = wangpan
TEMPLATE = app
INCLUDEPATH += src
SOURCES += \
    src/main.cpp \
    src/apiclient.cpp \
    src/transfer/transferutil.cpp \
    src/transfer/hashworker.cpp \
    src/transfer/uploadtask.cpp \
    src/ui/theme.cpp \
    src/ui/loginwindow.cpp \
    src/ui/mainwindow.cpp
HEADERS += \
    src/apiclient.h \
    src/transfer/transferutil.h \
    src/transfer/hashworker.h \
    src/transfer/uploadtask.h \
    src/ui/theme.h \
    src/ui/loginwindow.h \
    src/ui/mainwindow.h
