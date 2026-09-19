# End User Licence Agreement — Auscultate

**Developed by Delta Creation Co.**
Version 1.0 — 19 September 2026

> **Draft.** Written to be a lawyer's editing job rather than a lawyer's
> starting point. It has not been reviewed by one. See §12.

By installing or using Auscultate ("the Software") you agree to this
Agreement. If you do not agree, do not install or use it.

## 1. Licence

Delta Creation Co. ("we", "us") grants you a personal, non-exclusive,
non-transferable, revocable licence to install and use the Software on any
number of computers you own or control, for any purpose including commercial
work.

Audio you create, process or export with the Software is **yours**. We claim
no rights over your recordings, your edits, or anything you produce. There is
no royalty, no attribution requirement, and no restriction on what you may do
commercially with your own output.

## 2. What you may not do

You may not:

- redistribute, sell, rent, sublicense or host the Software as a service;
- remove or alter any copyright, trade mark or attribution notice;
- represent the Software as your own work.

## 3. Reverse engineering, and an important exception

Except as permitted by §4 and by applicable law, you may not decompile,
disassemble or reverse engineer the Software.

**This restriction does not apply to the third-party components listed in §4
that are licensed under the GNU Lesser General Public Licence.** For those
components you may reverse engineer for debugging modifications you make to
them, as their licences expressly permit, and nothing in this Agreement
limits any right those licences grant you. Where this Agreement and one of
those licences conflict, the component's licence prevails for that component.

## 4. Third-party components

The Software includes components owned by others and used under their own
licences. Those licences govern those components, not this Agreement.

| Component | Licence | How it is used |
| --- | --- | --- |
| Qt (qtbase) | LGPL-3.0 | Dynamically linked |
| ALSA (Linux builds) | LGPL-2.1 | Dynamically linked |
| dr_flac | Unlicense | Compiled in |
| dr_mp3 | Unlicense | Compiled in |
| nlohmann/json | MIT | Compiled in |
| WiX Toolset | MS-RL | Part of the Windows installer file only |

The WiX Toolset is the program that builds the Windows `.msi`. None of it is
part of Auscultate itself, but the installer file contains its setup dialogs
and a small library of its code, so its licence travels with that file. Its
licence text is in the `licences` folder installed with the Software, and its
source is public.

Qt and ALSA are **dynamically linked and shipped as replaceable libraries**,
as the LGPL requires. You may replace them with your own compatible versions:
on Windows the Qt DLLs sit beside the executable and may be substituted
directly. The full text of every licence above is distributed with the
Software, in the `licences` folder beside the executable, together with a
notices file identifying each component and where its source may be obtained.
On request we will provide the information needed to relink the Software
against a modified version of an LGPL component, and the complete source of
that component for the exact version shipped, for no more than the cost of
providing it.

## 5. Ownership

The Software is licensed, not sold. Delta Creation Co. retains all right,
title and interest in it, excluding the third-party components in §4 and
excluding anything you make with it.

## 6. Price and availability

The Software is currently provided free of charge. We may introduce paid
versions or features in future. **Doing so will not disable or restrict a
version you already have**, and this Agreement does not entitle us to take
away something you already installed.

## 7. No warranty

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.

Plainly: this is an audio tool that modifies files. **Keep backups of
anything you care about.** We do not warrant that it is free of defects, that
its measurements are fit for any particular compliance purpose, or that it
will not lose or damage data.

## 8. Measurements are not certified

The Software reports loudness, true peak, reverberation and other figures.
These are implemented from published definitions but **have not been verified
against any certification body's conformance material**, and nothing in the
Software or its documentation claims conformance to EBU R128, ITU-R BS.1770,
ISO 3382, IEC 61260 or any other standard.

Do not rely on its output as the sole basis for a delivery that carries a
contractual compliance requirement without checking it against a tool that is
certified.

## 9. Limitation of liability

To the fullest extent permitted by law, Delta Creation Co. shall not be liable
for any indirect, incidental, special, consequential or punitive damages, or
for any loss of profits, revenue, data or audio material, arising out of your
use of or inability to use the Software.

Where liability cannot lawfully be excluded, it is limited to the greater of
the amount you paid for the Software or £50.

Nothing in this Agreement excludes liability for death or personal injury
caused by negligence, for fraud, or for anything else that cannot lawfully be
excluded.

## 10. Termination

This licence ends automatically if you breach it. You may end it at any time
by uninstalling the Software. §5, §7, §9 and §11 survive termination.

## 11. Governing law

*Jurisdiction to be inserted before publication.* Nothing here affects
statutory consumer rights you have in your country of residence.

## 12. Changes, contact, and a caveat

We may revise this Agreement for future versions. The version accompanying a
release governs that release; a later Agreement does not retroactively change
the terms of a copy you already have.

Delta Creation Co. — *contact address to be inserted before publication.*

**This is a draft.** It has not been reviewed by a solicitor. Two things in
particular need a professional eye before publication: the reverse-engineering
carve-out in §3, which exists because a blanket prohibition would conflict
with the LGPL terms of Qt and ALSA and could render that clause unenforceable
or put the distribution in breach; and the liability cap in §9, whose
enforceability depends on the jurisdiction named in §11.
