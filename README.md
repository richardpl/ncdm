NCDM: NCurses Download Manager
==============================

Copyright (c) 2017 Paul B Mahol

NCDM is TUI tool to download files over various protocols.

Features
--------

* Resume download
* Showing extra info
* Speed download control for each URL
* Bunch of protocols supported

Usage
-----

After starting, press F1 to display help.
Use a/A to enter new download URL.
Use S to stop/start all downloads.
Use i to display more info for selected item.
Use D to delete selected download from the download list.
Use HOME/END & UP/DOWN to scroll items being downloaded.
Use LEFT/RIGHT to decrease/increase speed of download.
Use Q to quit.

You can also give URLs you want to download via command-line parameters.

Optional switches:
-R referer - This one set referer for next URL. If URL does not follow it, it
             will be ignored.

-M number  - This one set max number of connections available at same time.
             Setting this to 0, will use new connection for each new download.
             Set this to 1 if you want to download up to one item at same time.

Building
--------

To build NCDM, you need C99 compiler, a POSIX system, recent libcurl
library and ncursesw library.
To build simply type `make`.

Bugs & Patches
--------------

Please file any bugs or problems on the GitHub Issues page, and send
any new code as a Pull Request.
