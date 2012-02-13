TEMPLATE = subdirs

IRIS_BASE = $$PWD

include(conf.pri)
windows:include(conf_win.pri)

include(common.pri)

# do we have a reason to enter the src dir?
appledns:!appledns_bundle:CONFIG *= build_src
!irisnetcore_bundle:CONFIG *= build_src
!iris_bundle:CONFIG *= build_src

sub_src.subdir = src
sub_tools.subdir = tools
sub_tools.depends = sub_src

build_src:SUBDIRS += sub_src

!no_tests:SUBDIRS += sub_tools
