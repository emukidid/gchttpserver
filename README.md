## GC HTTP Server

A small HTTP Server for the GameCube/Wii, smashed together using [picohttpparser](https://github.com/h2o/picohttpparser) and the network example from [libogc2](https://github.com/extremscorner/libogc2)

There are probably bugs, please contribute :)

### Building
Follow the Installation Instructions from [libogc2](https://github.com/extremscorner/libogc2) and once done, type 'make' and 'make -f Makefile.wii' in the root of this codebase.

### Features
* /storage serves contents of first available storage
* /status page shows stats
* / mounts to /www dir on first available storage
* Concurrent access support (8 threads)
* Attempts to cache frequently accessed files (up to 1MB worth)
* Streams larger files (> 512KB) directly from storage
* Works with the BBA or any other supported network adapter from libogc2

### Usage
* Boot the .dol, your IP will be displayed on the screen
* Navigate to http://<ip>:8080/status
* If you would like files served, have some storage available and go /storage or / (if you have a /www dir on the storage).