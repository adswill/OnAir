# Licence texts kept in this repository

The SDK copies these into every tarball, next to the licences that come with the packages themselves.

* `common-licenses/`: the standard licence texts of Ubuntu 26.04 (`/usr/share/common-licenses` of the package
  `base-files` 14ubuntu6.3). Ubuntu's `copyright` files refer to them instead of repeating them; MSYS2 packages that ship
  no licence file are under one of them too. They go to `licenses/common-licenses/` in the SDK.
* `llvm/LICENSE.TXT`: LLVM's licence (Apache-2.0 WITH LLVM-exception), from the tag `llvmorg-23.1.5` of
  https://github.com/llvm/llvm-project (`llvm/LICENSE.TXT`, sha256 8d85c105...). LLVM's release tarball holds no licence file.
  Check it again when `LLVM_VERSION` in `common/fetch_toolchain.sh` changes.
