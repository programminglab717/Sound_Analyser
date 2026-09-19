# Installing Auscultate on Windows

**Developed by Delta Creation Co.**

Every CI run produces two Windows downloads, and you can have either:

| Artefact | What it is |
| --- | --- |
| `auscultate-windows-installer` | `auscultate-<version>-x64.msi` and its SHA-256. Installs, adds a Start menu entry, and uninstalls cleanly |
| `auscultate-windows` | The same files in a folder. Unzip it anywhere and run `auscultate.exe`. Nothing is installed and nothing is written outside the folder except the cache and settings below |

Both contain identical binaries. Use whichever suits you.

---

## Windows will warn you, and here is why

**Auscultate is not code-signed.** When you run the installer, Windows shows a
blue box headed **"Windows protected your PC"**, saying Microsoft Defender
SmartScreen prevented an unrecognised app from starting. The only obvious
button is **Don't run**.

To continue: click **More info**, then **Run anyway**. Your browser may also
warn while downloading, because the file is not commonly downloaded and carries
no signature; choose to keep the file.

This is a deliberate decision, not an oversight:

- A code-signing certificate costs money every year, and this product is built
  under a rule that nothing in it may cost money.
- **There is no free certificate authority for code signing.** The free CAs
  that issue TLS certificates do not issue code-signing certificates, and since
  2023 the ones that do are required to deliver the key on hardware, which
  raises the floor rather than lowering it.
- The free code-signing programmes that do exist are for open-source projects
  and require the source to be public. Auscultate is closed-source, so it does
  not qualify. That door is closed, not merely unopened.

**What the warning does and does not mean.** SmartScreen is telling you it has
not seen this file often enough to vouch for it. It is not a virus report. It
would say the same about the first download of any new unsigned program.

**It will not go away on its own.** For unsigned files SmartScreen's reputation
is tracked per file, so every new build starts from nothing. A signed product
accumulates reputation against the publisher instead, which is the actual thing
a certificate buys.

### What we do instead, which is free

- **Every build publishes a SHA-256.** It is written next to the `.msi` as
  `auscultate-<version>-x64.msi.sha256` and printed in the build's summary page
  by the job that produced it, so the number does not come from the same place
  as the file. Check it before installing:

  ```powershell
  Get-FileHash .\auscultate-0.1.0-x64.msi -Algorithm SHA256
  ```

  That tells you the file you have is the file the build produced. It cannot
  tell you who produced it — only a signature does that.

- **Publisher details are inside the binaries.** Right-click `auscultate.exe`,
  choose Properties, then Details: company, product, version and copyright are
  filled in. Treat this as a courtesy, not as evidence: anyone can write any
  name into a version resource.

- **Worth trying, not yet tried:** submitting the installer to the
  community `winget` package repository. It is free, requires no certificate,
  and lets people install with a single command instead of a browser download,
  which removes the download warning. It does **not** make the program signed,
  and we have not been through that process, so nothing here promises it works.

We will not ask you to disable SmartScreen or add an antivirus exclusion. If
you are not comfortable running an unsigned program, the right thing to do is
not to run it.

---

## What the installer does

- Installs to **`%LOCALAPPDATA%\Programs\Auscultate`** — your own account only.
  **No administrator password is needed**, and there is no UAC prompt.
- Adds **Auscultate** to the Start menu.
- Registers an entry in **Settings → Apps → Installed apps** under the
  publisher **Delta Creation Co.**, with the version number.
- Shows the End User Licence Agreement before installing.

It does **not** add anything to `PATH`, set file associations, install a
service, add a scheduled task, or make a network connection.

`auscultate-cli.exe`, the headless batch driver, is installed in the same folder.
To use it from a terminal, either call it by full path or add that folder to
your own `PATH`.

### Installing somewhere else, or silently

```powershell
# Silent
msiexec /i auscultate-0.1.0-x64.msi /qn

# A different folder
msiexec /i auscultate-0.1.0-x64.msi INSTALLFOLDER="D:\Audio\Auscultate"

# With a log, if something goes wrong
msiexec /i auscultate-0.1.0-x64.msi /l*v install.log
```

### Upgrading

Install the newer `.msi`. It replaces the older version rather than sitting
beside it. Going backwards is refused with a message; uninstall first if you
mean to.

---

## Uninstalling

Settings → Apps → Installed apps → Auscultate → Uninstall. Or:

```powershell
msiexec /x auscultate-0.1.0-x64.msi
```

**What the uninstaller removes:** every file it installed, the Start menu
shortcut, the folder it created, and its own registry entries. Nothing of the
installation is left.

**What it deliberately leaves alone** — your data, written by the application
rather than by the installer:

| What | Where |
| --- | --- |
| Window size and preferences | `HKEY_CURRENT_USER\Software\Delta Creation Co.\Auscultate` |
| Spectrogram cache | `%LOCALAPPDATA%\Auscultate\spectrogram-tiles` |

Uninstalling does not touch either, because reinstalling later should find your
settings where you left them, and because an uninstaller that deletes a user's
data is a worse fault than one that leaves a folder behind. To remove them, delete
`%LOCALAPPDATA%\Auscultate` and that registry key. See `PRIVACY.md`, which ships
beside the application, for what is in them.

Session files and exported audio are wherever you saved them and are never
touched.

---

## Replacing the Qt libraries

Auscultate uses Qt under the LGPL-3.0, which gives you the right to run it
against your own build of Qt. The Qt DLLs are ordinary files in the install
folder and you may replace them with compatible ones.

Two things about this installer exist to keep that right real:

- Installing into `%LOCALAPPDATA%` rather than `Program Files` means you can
  replace them **without an administrator**.
- The Start menu shortcut is a plain shortcut, not an *advertised* one, and no
  component is keyed to a DLL. Windows Installer therefore has no reason to
  "repair" the product when it finds a DLL it did not put there, so a library
  you replaced stays replaced.

If you need the information to relink Auscultate against a modified Qt, ask.
The obligation is in §4 of the EULA and we intend to meet it.

---

## Notes on the documents that ship with it

`EULA.md` and `PRIVACY.md` are installed beside the application, and the EULA is
what the installer's licence page shows. **Both are drafts.** They were written
by reading what the software actually does, but neither has been reviewed by a
solicitor, and both say so at the foot of the document. Two clauses in
particular are flagged there as needing a professional eye before publication.

The `licences` folder holds the licence text for the WiX Toolset, whose code is
embedded in the `.msi` itself. The full licence texts for Qt, dr_libs and
nlohmann/json are **not yet** shipped alongside the binaries — they are listed
with their licences in §4 of the EULA, but LGPL-3.0 expects its own text to
travel with the product, and that is an outstanding gap rather than a decision.
