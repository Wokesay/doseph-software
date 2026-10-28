# Security

## Reporting a vulnerability – not in public

Please **never as a public issue**. Report a vulnerability through
GitHub's **private vulnerability reporting** (you need a GitHub account):
<https://github.com/Wokesay/doseph-software/security/advisories/new>.

Helpful: the affected version or commit, steps to reproduce (the
simulator is often enough) and the possible consequences. Especially
important: anything that can make a pump run unintentionally – above all
a dose without flow – and anything that bypasses the sign-in.

## What you can expect

| Step | Target |
|---|---|
| Acknowledgement of receipt | 7 days |
| First assessment | 30 days |
| Fix | as soon as possible, in the next prototype |
| Disclosure | after the fix, coordinated with you; at the latest 90 days after the report |

Every fixed vulnerability gets a GitHub security advisory in this
repository, with a CVE where GitHub assigns one. A vulnerability here is
a flaw that someone without the needed rights can trigger: a device in
the network, a crafted file or another input. The release notes list
every security item.

We will not take action against anyone who researches and reports in
good faith: on their own installation or the simulator only, without
accessing third-party devices or data, without denial of service, and
keeping the details confidential until disclosure.

## What the software keeps

- A mandatory password at the first start, with a setup code the device
  shows; no default password. Passwords are stored as salted PBKDF2
  hashes; a lockout per client after failed attempts.
- Access tokens for an app, the account or a coupled device, each with
  a fixed role that limits what it may call. A token is shown once and
  kept as a hash; the owner revokes it in the settings (SD-129).
- Host and origin checks against DNS rebinding and cross-site requests;
  the session in an `HttpOnly`, `SameSite=Strict` cookie.
- Only the gateway switches pumps; no dose without proven flow; a crash
  or restart switches everything off (`docs/CONCEPT.md`).

## Supported versions

| Version | Security updates |
|---|---|
| 0.x prototypes | only the latest prototype: a fix comes in the next one |

Prototypes are for trying the software in the simulator. Do not dose
into a real tank with them.
