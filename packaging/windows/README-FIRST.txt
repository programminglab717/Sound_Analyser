Auscultate
Developed by Delta Creation Co.


START HERE
==========

Extract this whole folder somewhere first -- your Desktop or Documents is
fine. Then double-click:

    Run Auscultate.bat

Not auscultate.exe. The script clears the "came from the internet" mark that
Windows puts on downloaded files and then starts the application. Running the
exe directly is what produces the warning.

Windows may show "Open File - Security Warning" for the script itself. That
dialog has a Run button, and it is a different and much weaker warning than
the one on the exe.


IF YOU STILL CANNOT RUN IT
==========================

There are two separate Windows features that block unsigned applications, they
look similar, and they need different answers. Which one you have is worth
thirty seconds to establish, because the fix for one does nothing for the other.

Look at what you were shown.

  A blue box saying "Windows protected your PC", with a "More info" link that
  reveals a "Run anyway" button
      -> This is SmartScreen. The script above fixes it. If you would rather
         do it by hand: right-click the zip you downloaded, choose Properties,
         tick Unblock at the bottom, press OK, and extract it again.

  A box with no "More info" link and no way past it, possibly mentioning Smart
  App Control
      -> This is Smart App Control, and nothing in this folder can get past it.
         See below.


SMART APP CONTROL
=================

Smart App Control refuses any application that is not signed by a certificate
it already trusts, and it offers no override. There is no file, script or
setting inside this folder that changes that -- the decision is made before
anything here runs.

Auscultate is not signed. Code-signing certificates are sold, typically for a
few hundred pounds a year, and this product is built without spending money,
so there is no certificate to sign it with. A self-signed certificate does not
help here either: Smart App Control wants a signature that Microsoft's own
trust list recognises, which is exactly what cannot be had for free.

To check whether it is on:

    Windows Security
      -> App & browser control
        -> Smart App Control

It will read On, Off, or Evaluation.

If it reads On, your options are:

  1. Turn Smart App Control off.

     This is a real decision and worth understanding before you make it. Smart
     App Control cannot be turned back on afterwards without reinstalling
     Windows -- Microsoft made it one-way deliberately. Everything else in
     Windows Security stays on: Defender still scans, SmartScreen still warns,
     the firewall is untouched. You would be giving up one extra layer that
     refuses unknown applications outright, and it is the layer that is
     stopping this one.

  2. Run Auscultate on a different machine that does not have it on. Smart App
     Control is only switched on by default on Windows 11 machines that were
     set up from a clean install; a machine upgraded from Windows 10 has it
     off.

  3. Build Auscultate from source on your own machine. See docs/BUILDING.md.
     This takes a working compiler and Qt and is a much longer road, but the
     result is a binary that was never downloaded.


WHAT IS IN THIS FOLDER
======================

  auscultate.exe        The application.
  auscultate-cli.exe    The same measurements from a command line, for scripts.
  Qt6*.dll, platforms\  Qt, which the application needs to draw its window.
                        These are shipped as separate replaceable files
                        because Qt's licence requires it.


WHAT THIS APPLICATION DOES NOT DO
=================================

It does not connect to the internet. Not for updates, not for telemetry, not
for anything -- it contains no networking code at all and cannot make a
connection. Nothing you open in it leaves your machine. See docs/PRIVACY.md.
