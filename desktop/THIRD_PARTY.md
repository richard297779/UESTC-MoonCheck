# Third-party and protocol notices

MoonCheck desktop code and the C++ firmware used for this port are covered by the repository MIT license. The Python implementation in `wechat_wire.py` and `wechat_transport.py` is a port of this repository's firmware, not a copy of external Python SDK sources.

Protocol references: [YYB-Go-Enhanced](https://github.com/525815266/YYB-Go-Enhanced) and [moon-uestc](https://github.com/MioYiSama/moon-uestc), already referenced by firmware. Public application IDs, protocol labels and server public keys are interoperability constants; they are not user credentials. The latter repository did not provide an explicit license at inspected commit `f8a93c3bc4d62cea879b76560c49c9646387b386`, so its Python files are not redistributed.

PyCryptodome supplies AES-GCM and P-256. It includes BSD-licensed and public-domain components; the exact distribution license is shipped in built packages. [PyCryptodome license](https://www.pycryptodome.org/src/license).

PyInstaller is a build tool, licensed under GPL with its bootloader exception allowing distribution of packaged programs under their own license. Its license/exception files are included in built distributions. [PyInstaller license](https://pyinstaller.org/en/stable/license.html).
