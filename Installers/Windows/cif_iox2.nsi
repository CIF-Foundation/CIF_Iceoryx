; CIF Iceoryx Windows installer
; Installs iox2_cif.dll to a fixed location under Program Files.

!include "MUI2.nsh"

!define PRODUCT_NAME "CIF Iceoryx"
!define PRODUCT_VERSION "0.1.1"
!define PACKAGE_NAME "cif-iox2"
!define PRODUCT_PUBLISHER "Dome Automation"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\CIF_IOX2"
!define INSTALL_DIR "$PROGRAMFILES64\CIF_Foundation\Libraries"
!define UNINSTALLER_NAME "${PACKAGE_NAME}-Uninstall.exe"
!define LICENSE_FILE "..\LICENSE-APACHE"

Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "${PACKAGE_NAME}-${PRODUCT_VERSION}.exe"
InstallDir "${INSTALL_DIR}"
RequestExecutionLevel admin
ShowInstDetails show
ShowUnInstDetails show

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${LICENSE_FILE}"
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TEXT "Setup has successfully installed ${PRODUCT_NAME}.$\r$\n$\r$\nIf you are using Iceoryx from LabVIEW you should also install the CIF Iceoryx package using VIPM."
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "${INSTALL_DIR}"
  File "resource\iox2_cif.dll"
  File "${LICENSE_FILE}"
  File "..\NOTICE"

  WriteUninstaller "${INSTALL_DIR}\${UNINSTALLER_NAME}"

  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "UninstallString" "$\"${INSTALL_DIR}\${UNINSTALLER_NAME}$\""
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "InstallLocation" "${INSTALL_DIR}"
  WriteRegDWORD HKLM "${PRODUCT_UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${PRODUCT_UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  Delete "${INSTALL_DIR}\iox2_cif.dll"
  Delete "${INSTALL_DIR}\LICENSE-APACHE"
  Delete "${INSTALL_DIR}\NOTICE"
  Delete "${INSTALL_DIR}\${UNINSTALLER_NAME}"
  DeleteRegKey HKLM "${PRODUCT_UNINST_KEY}"
SectionEnd
