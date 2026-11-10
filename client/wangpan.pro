QT += core gui widgets network
CONFIG += c++11
CONFIG -= app_bundle
TARGET = wangpan
TEMPLATE = app
INCLUDEPATH += src
SOURCES += \
    src/main.cpp \
    src/apiclient.cpp \
    src/ui/theme.cpp \
    src/ui/loginwindow.cpp
HEADERS += \
    src/apiclient.h \
    src/ui/theme.h \
    src/ui/loginwindow.h
