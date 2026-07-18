include(../homed-common/homed-common.pri)
include(../homed-common/homed-color.pri)

HEADERS += \
    controller.h \
    device.h

SOURCES += \
    controller.cpp \
    device.cpp

QT += network
