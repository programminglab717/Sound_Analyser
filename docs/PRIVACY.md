# Privacy Policy — Auscultate

**Developed by Delta Creation Co.**
Last updated: 19 September 2026

> **Draft.** This was written by reading what the software actually does, not
> from a template. It has not been reviewed by a lawyer. See §8.

## 1. The short version

Auscultate does not collect anything, does not send anything anywhere, and
does not require an account. It has no network code in it at all.

This is not a policy of restraint. The application is not built against any
networking library — it links Qt's widget module and nothing else — so it is
not capable of making a network connection. There is no telemetry to disable
because there is no telemetry to write.

## 2. What we collect

Nothing.

We do not collect your name, your email address, your IP address, your
location, device identifiers, usage statistics, crash reports, or the contents
of any audio you open.

## 3. What stays on your computer

Auscultate writes only what you ask it to write. It keeps no cache, no history,
no log and no settings file: close it and the only traces of your session are
the files you saved yourself.

| What | Where | Why |
| --- | --- | --- |
| Session files (`.sa`) | Wherever you save them | So a project can be reopened |
| Exported audio | Wherever you save it | It is the output you asked for |

That is the complete list, and it is short for a reason worth stating. Every
analysis this product performs -- the spectrogram, the loudness measurement, the
key, the tempo -- is done in memory and discarded when you close the file. A
long recording is therefore analysed again each time you open it, which costs
you a little time and means nothing about what you have been listening to is
left behind on the disk.

If a future version adds a cache or remembers your preferences, this section
will say so before that version ships, and will name the exact location.

## 4. What we share

Nothing, because we have nothing.

We do not sell data, share it with advertisers, or pass it to third parties.
There is no third party in this arrangement.

## 5. Children

Auscultate is a professional audio tool with no online component and no data
collection, so it poses no particular risk to children. We do not knowingly
collect information from anyone, of any age, because we do not collect
information.

## 6. Your rights

Data protection law gives you rights to access, correct, export and delete
personal data that an organisation holds about you. We hold none, so there is
nothing for us to produce or erase. Everything Auscultate writes is on your
own computer and under your own control.

## 7. If this ever changes

If a future version of Auscultate gains a feature that needs the network — an
update check, a licence activation, an online service — that is a change to
this policy and will be stated plainly, before the feature ships, with a way
to decline it. We will not add silent telemetry to a product whose policy says
it has none.

## 8. Contact, and a caveat

Questions about this policy go to Delta Creation Co.
*Contact address to be inserted before publication.*

**This is a draft.** It is accurate about what the software does as of the
date above, and that accuracy was checked against the source rather than
assumed. It is not legal advice and has not been reviewed by a solicitor. If
Auscultate ever takes payment, adds accounts, or gains any network feature,
have this reviewed before that ships.

*Governing law to be inserted before publication.*
