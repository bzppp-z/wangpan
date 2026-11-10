QT += testlib network
CONFIG += testcase console
CONFIG -= app_bundle
TARGET = test_apiclient
TEMPLATE = app
INCLUDEPATH += ../src
SOURCES += test_apiclient.cpp ../src/apiclient.cpp
HEADERS += ../src/apiclient.h
