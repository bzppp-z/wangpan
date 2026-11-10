QT += testlib
CONFIG += testcase console
TARGET = test_transfer
TEMPLATE = app
INCLUDEPATH += ../src/transfer
SOURCES += test_transfer.cpp ../src/transfer/transferutil.cpp
HEADERS += ../src/transfer/transferutil.h
