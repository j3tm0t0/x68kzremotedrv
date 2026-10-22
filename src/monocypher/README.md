# Monocypher 4.0.2 (vendored)

https://monocypher.org/ — Loup Vaillant 氏ほか。CC0 / 2-clause BSD のデュアルライセンス (`LICENCE.md`)。
`monocypher-4.0.2.tar.gz` (SHA-256 `38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c`) から
X25519 / Ed25519 (SHA-512 版) に必要な 4 ファイルだけを、改変せずに置いている:

- `monocypher.c` / `monocypher.h` : X25519 (`crypto_x25519`, `crypto_x25519_public_key`)、EdDSA の点演算
- `monocypher-ed25519.c` / `monocypher-ed25519.h` : SHA-512 と Ed25519 (`crypto_ed25519_sign` / `crypto_ed25519_check`)

ファームウェア (x68kzremotedrv の `accel` ブランチ、`src/monocypher/`) にも同じものを置く。
