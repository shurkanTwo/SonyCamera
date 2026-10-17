# Public fork review — 2026-10-17

Reviewed all public forks listed by the GitHub forks API: seven existing SonyCamera forks (excluding this newly created fork) and ten ASCOMSonyCameraDriver forks. Inspected published releases and all listed branches, rather than relying on repository updated timestamps.

No inspected fork had published releases. The only new native-driver branches were @totoantibes’s fix/survive-device-removal and fix/transfer-buffer-size, corresponding to upstream PR #16 and #17. The remaining native forks and all ASCOM forks match upstream branch heads or retain older history. This finding covers the public fork networks, not private builds or unrelated projects.

## Native-driver forks

- [totoantibes/SonyCamera](https://github.com/totoantibes/SonyCamera): New source branches for upstream PR #16/#17; no releases.
- [charliekempf/SonyCamera](https://github.com/charliekempf/SonyCamera): No newer branch found; no releases.
- [changedsoul/SonyCamera](https://github.com/changedsoul/SonyCamera): No newer branch found; no releases.
- [SchroedingerHepcat/SonyCamera](https://github.com/SchroedingerHepcat/SonyCamera): No newer branch found; no releases.
- [jmyers0022/SonyCamera](https://github.com/jmyers0022/SonyCamera): No newer branch found; no releases.
- [594270461/SonyCamera](https://github.com/594270461/SonyCamera): No newer branch found; no releases.
- [guopeng1314/SonyCamera](https://github.com/guopeng1314/SonyCamera): No newer branch found; no releases.

## ASCOM forks

- [charliekempf/ASCOMSonyCameraDriver](https://github.com/charliekempf/ASCOMSonyCameraDriver): no newer maintained version or release found.
- [changedsoul/ASCOMSonyCameraDriver](https://github.com/changedsoul/ASCOMSonyCameraDriver): no newer maintained version or release found.
- [SchroedingerHepcat/ASCOMSonyCameraDriver](https://github.com/SchroedingerHepcat/ASCOMSonyCameraDriver): no newer maintained version or release found.
- [linqing51/ASCOMSonyCameraDriver](https://github.com/linqing51/ASCOMSonyCameraDriver): no newer maintained version or release found.
- [randeng/ASCOMSonyCameraDriver](https://github.com/randeng/ASCOMSonyCameraDriver): no newer maintained version or release found.
- [Mxiaotao/ASCOMSonyCameraDriver](https://github.com/Mxiaotao/ASCOMSonyCameraDriver): no newer maintained version or release found.
- [BenLand100/ASCOMSonyCameraDriver](https://github.com/BenLand100/ASCOMSonyCameraDriver): no newer maintained version or release found.
- [JackZhangCHN/ASCOMSonyCameraDriver](https://github.com/JackZhangCHN/ASCOMSonyCameraDriver): no newer maintained version or release found.
- [devilmastah/ASCOMSonyCameraDriver](https://github.com/devilmastah/ASCOMSonyCameraDriver): no newer maintained version or release found.
- [rohit-vinod-rv/ASCOMSonyCameraDriver](https://github.com/rohit-vinod-rv/ASCOMSonyCameraDriver): no newer maintained version or release found.

## Decision

Continue a maintained native-driver build and review upstream PR #17 with its original commit history. Keep PR #16 separate for device-lifecycle review and physical-camera testing. Do not fork or redistribute a modified ASCOM wrapper while its License TBD statement remains unresolved.
