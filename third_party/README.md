# SHA-256 dependency

MiniGit vendors **PicoSHA2 v1.0.1** by okdshin, an MIT-licensed, header-only SHA-256 library.

- Upstream: https://github.com/okdshin/PicoSHA2
- Header: https://raw.githubusercontent.com/okdshin/PicoSHA2/v1.0.1/picosha2.h
- License: https://raw.githubusercontent.com/okdshin/PicoSHA2/v1.0.1/LICENSE
- Unmodified header SHA-256: `b13c180161ffac8d0adc81e033e493c409457c4d1258ab9781ac80579ba3bdd8`
- License file SHA-256: `6c30eb1f37554ec4199cb82a8c86d9e7a852da78a757832bebb03ac9ddc44ff1`

CMake includes this directory as a system include directory for `minigit_core`. Nothing is downloaded while building. There is no OpenSSL or package-manager requirement. The application calls `picosha2::hash256_hex_string`; it does not implement cryptography itself. Tests compare known SHA-256 vectors and independently verify serialized object hashes with Python's `hashlib`.

Third-party coding style belongs to the upstream library; the educational application source lives in `src/` and `include/minigit/`.
