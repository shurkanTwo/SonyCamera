# Attribution and third-party software

SonyCamera originates at https://github.com/dougforpres/SonyCamera by @dougforpres and its contributors. This community fork is maintained by @shurkanTwo. The transfer-buffer fix is @totoantibes's original upstream PR #17 commit, preserved in history. SonyCamera remains under its original GPLv3 license; see LICENSE.

Release packages dynamically link these libraries:

- LibRaw 0.21.0, source commit `979160ff13c89e1158b1995975c23ed7497b6b62`, https://github.com/LibRaw/LibRaw. This distribution uses the LGPL 2.1 license option. LibRaw's original copyright and component notices and both offered license texts are preserved in THIRD-PARTY-LICENSES/LibRaw and the matching source archive.
- libjpeg-turbo 2.0.3, source commit `5db6a6819d0f904e0b58f34ae928fea234adb1a0`, https://github.com/libjpeg-turbo/libjpeg-turbo. IJG, BSD-style and zlib license texts and notices are preserved in THIRD-PARTY-LICENSES/libjpeg-turbo and the matching source archive. This software is based in part on the work of the Independent JPEG Group.

The source archive alongside each release contains the corresponding SonyCamera and dependency source and build scripts. See SOURCE.txt in each binary package. LibRaw and turbojpeg remain separate DLLs that users can replace with compatible builds. Original legal notices are reproduced verbatim as required, including their original copyright names; community credit prose uses GitHub handles.

These archives do not contain or modify the separately maintained ASCOMSonyCameraDriver wrapper. Its license status is independent of this native project.
