# SonyCamera

## Community maintenance

This is @shurkanTwo's maintenance fork of [@dougforpres/SonyCamera](https://github.com/dougforpres/SonyCamera). @dougforpres and the original contributors retain credit for the project. Original commits and copyright notices are preserved.

The maintained branch is **workerThread**. [Downloads](https://github.com/shurkanTwo/SonyCamera/releases), [manual installation and rollback](INSTALLATION.md), [build instructions](build/README.md), [changes](RELEASE-NOTES.md), and [upstream PR #17 review](docs/REVIEW-PR17.md) are available here. The current native release, **1.0.1.22**, is a regular GitHub release marked Latest. Its original `1.0.1.22-preview.1` tag and download filenames are retained to preserve existing links. Windows CI does not establish physical-camera or ASCOM/NINA compatibility. Experimental PR #16 device-removal work remains separate from this release.

This fork maintains the GPLv3 native DLL and diagnostic tool. The separate ASCOM wrapper is not included in these packages. See the [public fork review](docs/FORK-REVIEW.md) for other existing forks and the work that remains separate.

## Original project description

Windows DLL Connecting to Sony Camera via USB

1st draft of README. For now, see https://retro.kiwi/

This DLL allows an application to connect to a compatible Sony DSLR/Mirrorless camera.
Individual camera settings (Properties) can be retrieved and updated.
Images can be taken and retrieved.

This software was developed primarily for Astrophotography purposes, and a companion project ASCOMSonyCameraDriver provides a basic ASCOM Driver.  https://github.com/dougforpres/ASCOMSonyCameraDriver

This is the first c++ code I've written since 2005/2006, so please don't judge me :)

See [Wiki pages](https://github.com/dougforpres/ASCOMSonyCameraDriver/wiki) of the companion ASCOM driver for install info

## Dependencies
https://www.libraw.org
Being used to convert received ARW files to RGGB and RGB format

https://www.libjpeg-turbo.org/
Being used to convert received JPEG files to RGB format
