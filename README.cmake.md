Files to build Psi+ using cmake utility.

HOWTO USE:

Instead of PSI_SOURCES_PATH must be your real psi_sources path

> $ cp -rf * PSI_SOURCES_PATH/

> $ cd PSI_SOURCES_PATH && mkdir build && cd build

> $ cmake -DCMAKE_INSTALL_PREFIX=/usr ..

> $ make && make install

USEFULL CMAKE FLAGS:

  -DUSE_QT5=ON
  
  to build psi-plus with Qt5 support (default ON)

  -DENABLE_PLUGINS=ON

  to build psi-plus plugins (default OFF)

  -DONLY_PLUGINS=ON

  to build only psi-plus plugins (default OFF)

  -DBUNDLED_IRIS=ON

  to build iris library bundled (default ON)

  -DUSE_ENCHANT=ON
  
  to use Enchant spellchecker (default OFF)
  
  -DUSE_HUNSPELL=ON
  
  to use Hunspell spellchecker (default ON)
  
  -DSEPARATE_QJDNS=ON

  to build qjdns library as separate library (default ON)
  
  -DPSI_PLUS_VERSION=${version}
  
  to set Psi-plus version manually

  -DBUILD_PLUGINS=${plugins}

  set list of plugins to build. To build all plugins:  -DBUILD_PLUGINS="ALL" or do not set this flag

  - possible values for ${plugins}:

    historykeeperplugin	stopspamplugin juickplugin translateplugin gomokugameplugin attentionplugin
    cleanerplugin autoreplyplugin contentdownloaderplugin	qipxstatusesplugin skinsplugin icqdieplugin
    clientswitcherplugin captchaformsplugin watcherplugin videostatusplugin screenshotplugin
    jabberdiskplugin storagenotesplugin	extendedoptionsplugin imageplugin	extendedmenuplugin
    birthdayreminderplugin gmailserviceplugin gnupgplugin pepchangenotifyplugin otrplugin
    chessplugin conferenceloggerplugin gnome3supportplugin enummessagesplugin
  
  Example:
  
  > $ cmake -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_PLUGINS="chessplugin;otrplugin;gnome3supportplugin" ..



  -DPLUGINS_PATH=${path} 

  to install plugins into ${path}. To install into default suffix:

  -DPLUGINS_PATH=lib/psi-plus/plugins or do not set this flag

  For example to install plugins into ~/.local/share/psi+/plugins:

  > $ cmake -DCMAKE_INSTALL_PREFIX=$HOME/.local -DPLUGINS_PATH=share/psi+/plugins ..

  For example to install plugins into /usr/share/psi-plus/plugins:

  > $ cmake -DCMAKE_INSTALL_PREFIX=/usr -DPLUGINS_PATH=share/psi-plus/plugins ..

 
  -DCMAKE_BUILD_TYPE=Release or -DCMAKE_BUILD_TYPE=Debug

To build Psi-plus in OS QWINDOWS you need to set additional variables

  -DQCA_DIR=DIRECTORY
  
  to set Qca directory (WIN32)
  
  -DIDN_ROOT=DIRECTORY
  
  to set Idn directory (WIN32)
  
  -DZLIB_ROOT=DIRECTORY

  to set Zlib directory (WIN32)
  
  -DHUNSPELL_ROOT=DIRECTORY
  
  to set Hunspell directory (WIN32)
  
  -DPRODUCTION=ON
  
  to install needed libs to run Psi+ (WIN32)

To build OTRPLUGIN in OS WINDOWS you need to set additional variables

- path to LIBGCRYPT:

  -DLIBGCRYPT_ROOT=%LIBGCRYPT_ROOT%

- path to LIBGPG-ERROR

  -DLIBGPGERROR_ROOT=%LIBGPGERROR_ROOT%

- path to LIBOTR

  -DLIBOTR_ROOT=%LIBOTR_ROOT%

- path to LIBTIDY

  -DLIBTIDY_ROOT=%LIBTIDY_ROOT%

  For example:

  > $ cmake -DLIBGCRYPT_ROOT=C:\libgcrypt -DLIBGPGERROR_ROOT=C:\libgpg-error -DLIBOTR_ROOT=C:\libotr -DLIBTIDY_ROOT=C:\libtidy ..

If you use Psi+ SDK set SDK_PATH (WIN32)

  -DSDK_PATH=path


  
TODO LIST:
- Add MacOSX support
