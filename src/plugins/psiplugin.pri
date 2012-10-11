TEMPLATE = lib
CONFIG += plugin
QT += xml

target.path = $$(HOME)/.local/share/psi+/plugins
INSTALLS += target

include(plugins.pri)
