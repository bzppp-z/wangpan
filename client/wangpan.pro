QT += core gui widgets network
CONFIG += c++11
CONFIG -= app_bundle
TARGET = wangpan
TEMPLATE = app
INCLUDEPATH += src
SOURCES += src/main.cpp src/apiclient.cpp
HEADERS += src/apiclient.h
