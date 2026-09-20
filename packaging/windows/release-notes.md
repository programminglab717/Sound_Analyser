**Auscultate __TAG__** — a spectrogram editor and measurement tool for Windows.
Developed by Delta Creation Co.

### Installing

Download the zip below, unzip it anywhere, and run **`Run Auscultate.bat`** —
not the `.exe`. The launcher clears the mark Windows attaches to downloaded
files, which is what causes the *"Windows protected your PC"* warning.
`README-FIRST.txt` sits beside it and explains what to do if a block remains.

This build is **not code-signed**, so Windows will warn about it. That warning
reports the absence of a certificate; it is not a detection of anything. There
is no free way to obtain a code-signing certificate, so this is a deliberate
trade rather than an oversight.

**Windows 10 and Windows 11 do not behave the same way here, and the difference
is not small.** This build has been seen running on Windows 10 and refused
outright by a Windows 11 machine with no override offered. Windows 11 carries
Smart App Control, which has no equivalent on Windows 10 and which rejects
unsigned applications before anything of ours runs; the launcher cannot help
with it. If you are blocked with no way past, check Windows Security ▸ App &
browser control ▸ Smart App Control, and see `README-FIRST.txt` in the zip.

### What is in the zip

`auscultate.exe`, the headless driver `auscultate-cli.exe`, the Qt libraries
they run on, the licence texts, and the two files above. Keep them together.

### What this does not claim

No measurement in this product has been checked against EBU or ITU conformance
material — those test vectors have never been run against it. The figures are
good engineering and are not a certified delivery check.

Playback has been confirmed audible on Windows through WASAPI. Other sample
rates, exclusive-mode devices and behaviour when a device is removed
mid-playback remain untested.

---

`SHA-256  __HASH__`
