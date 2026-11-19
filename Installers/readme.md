Instructions to build installers for RT (ipk), Windows (NSIS), and Ubuntu (deb).

Version is `VERSION` at the repo root. Each platform build writes a **versioned**
library into its own `dist\` folder. Installers install that file and create a
symlink named `libiox2_cif.so` / `iox2_cif.dll` (the LabVIEW Call Library name).

| Platform | Build output |
|---|---|
| Linux RT | `dist/linuxrt/libiox2_cif-linuxrt.so.<version>` |
| Ubuntu 20.04 | `dist/ubuntu/libiox2_cif-ubuntu.so.<version>` |
| Windows | `dist/windows/iox2_cif-windows-<version>.dll` |

## cif_iox2_ipk (LabVIEW RT)

1. Keep `VERSION` and `Installers/cif_iox2_ipk/CONTROL/control` in sync when cutting a release.
2. Build the RT library (core2, no AVX — required for Atom cRIO such as 9057):

   `powershell -ExecutionPolicy Bypass -File .\scripts\build-linuxrt.ps1 -Target <ssh-host>`

3. From Windows (needs WSL):

   `Installers\build_cif_iox2_ipk.bat`

   Or from WSL/Linux:

   `bash Installers/build_cif_iox2_ipk.sh`

`postinst` installs the versioned `.so` under `/usr/lib/cif/` and symlinks
`/usr/lib/cif/libiox2_cif.so` and `/usr/lib/libiox2_cif.so`.

## cif-iox2 Windows (NSIS)

1. Build the Windows library:

   `powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1`

2. Run:

   `Installers\Windows\build_nsis.bat`

That installs `iox2_cif-windows-<version>.dll` to
`C:\Program Files\CIF_Foundation\Libraries\` and creates `iox2_cif.dll` as a
symlink (copy fallback if `mklink` fails).

## cif-iox2 Ubuntu (deb)

Build on Ubuntu 20.04 (glibc 2.31):

`powershell -ExecutionPolicy Bypass -File .\scripts\build-ubuntu2004.ps1 -Target <ssh-host>`

Then:

`bash Installers/build_deb.sh`

`postinst` installs `libiox2_cif-ubuntu.so.<version>` under `/usr/lib/cif/` and
symlinks `libiox2_cif.so`.
