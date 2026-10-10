Instructions to build installers for RT (ipk), Windows (NSIS), and Ubuntu (deb).

These packages install the native iceoryx2 CIF library only. They do not compile
it. Build `dist\iox2_cif.dll` / `dist\libiox2_cif.so` first.

## cif_iox2_ipk (LabVIEW RT)

1. Update the version in `Installers/cif_iox2_ipk/CONTROL/control` when needed.
2. Build the RT library on a host that can compile NI Linux RT x64, using the
   portable `core2` ISA (no AVX — required for Atom cRIO targets such as 9057):

   `powershell -ExecutionPolicy Bypass -File .\scripts\build-linuxrt.ps1`

   The IPK build copies `dist/libiox2_cif.so` automatically. To override:

   `SO_SOURCE=/path/to/libiox2_cif.so bash Installers/build_cif_iox2_ipk.sh`

3. Build host needs `binutils`, `tar`, `gzip`, and `python3` (e.g. `sudo apt install binutils tar gzip python3`). The build uses the bundled `Installers/opkg-utils/opkg-build` script; Ubuntu/WSL does not provide an `opkg-utils` apt package.
4. From Windows, run:

   `Installers\build_cif_iox2_ipk.bat`

   Or from WSL/Linux:

   `bash Installers/build_cif_iox2_ipk.sh`

The build stages `Installers/cif_iox2_ipk/` into `Installers/staging/cif_iox2_ipk/`, normalizes text files to LF line endings, packages with `opkg-build`, and writes `cif-iox2_<version>_core2-64.ipk` to `Installers/output/`.

`postinst` installs the library under `/usr/lib/cif/` and creates a symlink at `/usr/lib/libiox2_cif.so`.

## cif-iox2 Windows (NSIS)

1. Update the version in `Installers/Windows/cif_iox2.nsi` when needed.
2. Build the Windows library:

   `powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1`

3. Run:

   `Installers\Windows\build_nsis.bat`

That copies `dist\iox2_cif.dll` into `Installers\Windows\resource\` if needed, then writes `cif-iox2-0.1.0.exe` next to the `.nsi` file. The installer places `iox2_cif.dll` in `C:\Program Files\CIF_Foundation\Libraries\`.

## cif-iox2 Ubuntu (deb)

1. Update the version in `Installers/cif_iox2_deb/DEBIAN/control` when needed.
2. Place `libiox2_cif.so` in `dist/` (or set `SO_SOURCE`).
3. From WSL/Linux:

   `bash Installers/build_deb.sh`

The build copies the template to a native Linux temp directory (so `dpkg-deb` sees `DEBIAN/` as `0755` instead of the `777` mode WSL reports on `/mnt/d`), then writes `cif-iox2_<version>_amd64.deb` to `Installers/output/`.
