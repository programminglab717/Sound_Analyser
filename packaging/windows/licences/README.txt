Third-party licence texts that travel with the Windows build.

WiX-Toolset-MS-RL.txt
  The WiX Toolset is the program that builds auscultate's .msi installer. None
  of it is linked into auscultate.exe or auscultate-cli.exe. The .msi itself,
  however, embeds WiX's standard setup-dialog bitmaps and its WixCA
  custom-action library, so the installer file does contain WiX code and the
  Microsoft Reciprocal License travels with it.

  Source for the exact version used: https://github.com/wixtoolset/wix3
  Licence text taken verbatim from that repository's LICENSE.TXT.

Qt, dr_libs and nlohmann/json are listed with their licences in the EULA that
ships beside this folder (section 4), and in third-party.json in the source
tree. Their full licence texts are NOT yet vendored here; see the note in
docs/INSTALLING.md.
