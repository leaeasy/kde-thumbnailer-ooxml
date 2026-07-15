cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

project(kde-thumbnailer-ooxml LANGUAGES CXX)

include(CTest)

set(QT_MIN_VERSION "6.8.0")
set(KF_MIN_VERSION "6.0.0")

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${ECM_KDE_MODULE_DIR})

include(KDEInstallDirs6)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDECMakeSettings)

find_package(Qt6 ${QT_MIN_VERSION} CONFIG REQUIRED COMPONENTS Gui Xml)
find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS Archive CoreAddons KIO)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(libkowmf_SRCS
    libkowmf/WmfAbstractBackend.cpp
    libkowmf/WmfDeviceContext.cpp
    libkowmf/WmfPainterBackend.cpp
    libkowmf/WmfParser.cpp
    libkowmf/WmfStack.cpp
    libkowmf/libkowmflogging.cpp
)

kcoreaddons_add_plugin(ooxmlthumbnail
    SOURCES
        legacyofficepreview.cpp
        ooxmlcreator.cpp
        ${libkowmf_SRCS}
    INSTALL_NAMESPACE "kf6/thumbcreator"
)
target_link_libraries(ooxmlthumbnail
    PRIVATE
        Qt6::Gui
        Qt6::Core
        Qt6::Xml
        KF6::Archive
        KF6::KIOGui
)

if(BUILD_TESTING)
    add_executable(ooxmlpreviewjob tools/ooxmlpreviewjob.cpp)
    target_link_libraries(ooxmlpreviewjob
        PRIVATE
            Qt6::Gui
            KF6::KIOGui
    )
endif()
