include(../../config-plugin.pri)

TARGET = tests-common
TEMPLATE = lib
CONFIG += staticlib

SOURCES += \
           utils.cpp \
           utils-gui.cpp \
           inputmethodhostprobe.cpp \
           wordengineprobe.cpp \

HEADERS += \
           utils.h \
           inputmethodhostprobe.h \
           wordengineprobe.h \

contains(QT_MAJOR_VERSION, 4) {
    QT = core gui
} else {
    QT = core gui widgets
}

INCLUDEPATH += ../../lib ../../
LIBS += $${TOP_BUILDDIR}/$${MALIIT_KEYBOARD_PLUGIN_LIB} $${TOP_BUILDDIR}/$${MALIIT_KEYBOARD_VIEW_LIB} $${TOP_BUILDDIR}/$${MALIIT_KEYBOARD_LIB}
PRE_TARGETDEPS += $${TOP_BUILDDIR}/$${MALIIT_KEYBOARD_PLUGIN_LIB} $${TOP_BUILDDIR}/$${MALIIT_KEYBOARD_VIEW_LIB} $${TOP_BUILDDIR}/$${MALIIT_KEYBOARD_LIB}

QMAKE_EXTRA_TARGETS += check
check.target = check
check.command = $$system(true)
check.depends += libtests-common.a