# Security

## Reporting a vulnerability

Never as an issue or in a pull request. This repository is private: report
a vulnerability directly to the repository owner, with the affected commit,
steps to reproduce (the simulator is often enough) and possible
consequences. Especially important: anything that can make a pump run
unintentionally – above all a dose without flow – and anything that
bypasses the sign-in.

## What the software keeps

- A mandatory password at the first start, with a setup code the device
  shows; no default password. Passwords are stored as salted PBKDF2 hashes; a lockout per
  client after failed attempts.
- Host and origin checks against DNS rebinding and cross-site requests;
  the session in an `HttpOnly`, `SameSite=Strict` cookie.
- Only the gateway switches pumps; no dose without proven flow; a crash or
  restart switches everything off (`docs/CONCEPT.md`).

## Supported versions

None yet: the software is in development and not released. Do not dose
into a real tank with it.

The predecessor growcontroller-software has its own policy and advisories
in its repository.
