TEMPLATE = app
TARGET = netRector
CONFIG += console c++17
CONFIG -= app_bundle
CONFIG -= qt
#baraye rafe ye bug #mohem search beshe

#kheyli mohem set beshe baraye proxy server
# اینطوری کانکشن‌های کوتاه‌مدت هزاران TIME_WAIT نمی‌سازن.
# net.ipv4.tcp_tw_reuse = 1
# net.ipv4.tcp_fin_timeout = 10

# ---------------------------------------------------------------------------
# BoringSSL (TLS)
#   scripts/build_boringssl.sh  ->  third_party/boringssl/{include,lib}
#   ya masire delkhah:  BORINGSSL_DIR=/opt/boringssl qmake
# ---------------------------------------------------------------------------
#BORINGSSL_DIR = $$(BORINGSSL_DIR)
#isEmpty(BORINGSSL_DIR): BORINGSSL_DIR = $$PWD/third_party/boringssl
#!exists($$BORINGSSL_DIR/include/openssl/ssl.h): error("BoringSSL not found in $$BORINGSSL_DIR (run scripts/build_boringssl.sh or set BORINGSSL_DIR)")

#INCLUDEPATH += $$BORINGSSL_DIR/include
#LIBS += -L$$BORINGSSL_DIR/lib -lssl -lcrypto -lpthread

unix:!macx {
    INCLUDEPATH += $$PWD/libs/boringssl/include
    DEPENDPATH += $$PWD/libs/boringssl/include

    LIBS += \
        $$PWD/libs/boringssl/libssl.a \
        $$PWD/libs/boringssl/libcrypto.a \
        -pthread -ldl
}

#LIBS += -luring
LIBS += -static -static-libgcc -static-libstdc++

#source Headers
INCLUDEPATH += $$PWD/src
DEPENDPATH += $$PWD/src


# ketabkhane: src/   |   barname haye test: root
SOURCES += \
        src/clsBufferPool.cpp \
        src/clsDNSLookup.cpp \
        src/clsEpollReactor.cpp \
        src/clsIntrusiveList.cpp \
        src/clsListener.cpp \
        src/clsLog.cpp \
        src/clsUDPSocket.cpp \
        src/clsMultiplexedTunnel.cpp \
        src/clsSendQueue.cpp \
        src/clsServer.cpp \
        src/clsSocketList.cpp \
        src/clsTCPSocket.cpp \
        src/clsTLSContext.cpp \
        src/clsTimer.cpp \
        src/clsTimerManager.cpp \
        src/tlsf.c \
        main.cpp

HEADERS += \
    src/SocketContext.h \
    src/clsBufferPool.h \
    src/clsDNSLookup.h \
    src/clsEpollReactor.h \
    src/clsIntrusiveList.h \
    src/clsListener.h \
    src/clsLog.h \
    src/clsReactorObject.h \
    src/clsUDPSocket.h \
    src/clsMultiplexedTunnel.h \
    src/clsSendQueue.h \
    src/clsServer.h \
    src/clsGCList.h \
    src/clsSocketList.h \
    src/clsTCPSocket.h \
    src/clsTLSContext.h \
    src/clsTimer.h \
    src/clsTimerManager.h \
    src/constants.h \
    src/epoll.h \
    src/tlsf.h \
    TestCommon.h \
    TunnelClient.h \
    Socks5LocalForwarder.h \
    Socks5StreamHandler.h \
    Socks5DirectProxy.h \
    HttpTest.h
