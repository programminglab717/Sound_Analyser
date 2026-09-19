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

Auscultate writes several things to your own disk. None of it leaves your
machine, and none of it is visible to us.

| What | Where | Why |
| --- | --- | --- |
| Session files (`.sa`) | Wherever you save them | So a project can be reopened |
| Exported audio | Wherever you save it | It is the output you asked for |
| Spectrogram cache | `%LOCALAPPDATA%\Auscultate\spectrogram-tiles` on Windows | So reopening a long file does not re-analyse it |
| Window size and preferences | The standard per-user settings location for your operating system | So the application opens as you left it |

The spectrogram cache holds analysed *pictures* of audio you have opened, not
the audio itself, and it is keyed by a hash of the file and the analysis
settings. It is bounded in size and evicts what has not been used recently.
You may delete the whole folder at any time; the application will rebuild what
it needs.

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
