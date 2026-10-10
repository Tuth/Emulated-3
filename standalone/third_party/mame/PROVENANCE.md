# Vendored MAME sources

Copied by `tools/vendor_mame.sh` from `/home/paul/EIII_RE/mame` at commit `eb3856b8de1fd1209bbfa1968916c658e0273f14`, **including the working-tree modifications**
listed in `LOCAL_CHANGES.diff` (the proven EIII SCSI behaviour: `ncr5380.cpp`, `nscsi_hle.cpp/.h`, `bus/nscsi/cd.cpp`).
Files are unmodified copies of that working tree; `MANIFEST.sha256` pins every byte.

Licences: every MAME file here is `license:BSD-3-Clause` (src/lib/util/disasmintf.cpp carries no tag; MAME's src/lib is
BSD-3-Clause). Berkeley SoftFloat 3e: see `3rdparty/softfloat3/COPYING.txt` (BSD-style, John R. Hauser).
No E-mu ROM, operating system, sample or bank data is included anywhere in this tree.
