# The account: a design

Step 10 of #10 begins with this design. Nothing in it is built yet. It
starts from the brief's frame for the account (PD-453). It names what an
account could offer and how it could work without taking control away
from the device. It also names what drives its running costs.

Two things are product questions, decided by the product session:
- how much the account offers;
- what its running costs may be.

Firmware updates have a question of their own (#62).

## The frame

These come from the brief and hold for every option below.

- **The device stays the authority.** Control runs on the device. An
  account adds reach, not control: no server decides a dose, a lock or a
  target. A server that is down changes nothing the device does. The local
  web app, push without an account (SD-124) and MQTT (SD-130) go on as
  before.
- **Optional.** Everything works offline, from the first start to daily
  use (commissioning, SD-129). An account is added later, and it can be
  removed again.
- **Outbound only.** The device connects out to a relay. Nothing is
  opened in the home network, and no port is forwarded.
- **End to end.** The relay forwards encrypted bytes. It sees neither
  readings nor recipes nor tokens.
- **Minimal data.** The account keeps only what it needs to find a
  device and to tell a phone it has news (below).
- **No subscription.** Whatever the account offers is paid with the
  device, or not at all.

## What an account could offer

| Offer | Without an account today | With an account |
|---|---|---|
| Remote access to the web app and the API | Only in the home network, or through the user's own VPN or Home Assistant | From anywhere, through the relay |
| Push | ntfy, Telegram, e-mail (SD-124), Home Assistant (SD-130) | The same; optionally web push or a store app (below) |
| Backup | Download and import by hand | Encrypted backups kept on the server, restored to a new device |
| Several devices | One web app per device; two devices pair on one tank (SD-131) | One list of the owner's devices |
| Updates | A product question of its own (#62) | – |

## How it could work

### Parts

- **The device** keeps one outbound connection to the relay, a WebSocket
  over TLS. It authenticates with a key pair made on the device; the
  private key never leaves it, and is kept with the device security
  items of #10.
- **The relay** joins a client's connection to its device's connection
  and forwards bytes. It knows a device by its public key. A client
  names the device by a host name of its own under the relay's domain,
  made from the device ID of SD-129. The relay routes by the name in the
  TLS handshake (SNI) and does not end TLS; it holds no keys for the
  content.
- **The account service** signs the owner in to the account (not to the
  device) and keeps:
  - the owner's devices, by device ID and public key;
  - the encrypted backups;
  - where present, push subscriptions.
- **The client** is a browser, or later an app. Its TLS ends on the
  device, which holds a certificate for its host name. So the web app a
  browser loads comes from the device itself, as in the home network,
  and not from a server. An app can also pin the device's key from the
  pairing. The relay is only the cable.

### Pairing a device with an account

1. In the local web app, signed in as the owner, the owner asks for a
   pairing code. The code is short-lived and shown once, like the coupling
   code of SD-131.
2. The owner enters it in the account. The device registers its public
   key and its ID with the account service, and that service lists the
   device for the owner.
3. From afar, a person signs in on the device as at home: with the
   password, or with an access token by the rules of SD-129. The device
   knows that a call came through the relay. Such a call gets at most the
   `account` role, unless the owner allows full use from afar in the
   local web app. Passwords and tokens go from the client to the device
   only, never to the account service.
4. Unpairing at the device ends the relay registration and every session
   from afar at once. Unpairing in the account removes the listing. The
   device reports it and goes on locally.

The roles stay as SD-129 has them. With the `account` role, a remote
client can read, use Stop, acknowledge alarms, and change recipes and
plants. It never reaches limits, releases, calibration, single pumps,
settings, the backup or maintenance; these stay at home, or from afar
only where the owner allowed full use. The relay and the account
service hold no password or token and can call nothing.

### Push through the account

There are three ways, from least to most to build and run:

- **A. None of its own.** Push stays with ntfy, Telegram, e-mail and Home
  Assistant (SD-124, SD-130). The account adds nothing here.
- **B. Web push.** The web app is installed on the phone as a web app,
  and the browser's push service delivers a message. The message is
  encrypted for that subscription (RFC 8291), so the push service sees
  only ciphertext. It needs an HTTPS origin, and the device's host name
  is one. The device can keep the subscription and send the push itself.
  The account service then sends only what the device cannot: "device
  not reachable". It needs no store app. Each platform and browser
  decides when web push is shown; this is to be checked when it is built.
- **C. A store app.** A native app receives push through the platforms'
  push services. This works most reliably, but needs an app in each store,
  with its review and developer accounts.

For B and C the alarm text can stay on the device. The push then says
only "new alarm on <device name>", and the client fetches the details
through the relay.

### Backup

- The device encrypts a backup (as `GET /backup` makes it) with a key
  from a passphrase the owner chooses. Only then is it uploaded.
- The account service keeps the last few backups per device. It cannot
  read them; a forgotten passphrase is a lost backup.
- As SD-129 says, a backup never carries the device ID, the access tokens
  or device secrets.

### What the account keeps

- The owner's sign-in (e-mail with a passkey or a password; to be chosen
  when built).
- Per device: its ID, its public key, a name the owner gives, when it was
  last connected, and the certificates issued for its host name.
- Encrypted backups, and push subscriptions where B or C is chosen.

Never: readings, recipes, plants, events, access tokens, or anything else
in clear.

### When the server is down

- **The device:** it goes on alone, as without an account.
- **The relay connection:** it is retried with growing waits, and the
  state says so.
- **Without the server:** remote access, account push and backup uploads
  stop. Local access, push without an account, MQTT and the local backup
  go on.

## Security notes

- **The main surface.** Remote access is the largest attack surface the
  product will have. The device enforces the roles itself. A captured
  relay or account service learns which devices exist and when they are
  online. It can call nothing and read nothing.
- **Keys and certificates.** The device's key pair and its certificate
  come with the device security items of #10: HTTPS per device, signed
  updates, encrypted storage, and factory and access reset. They are a
  precondition of the device side.
- **The certificate for the host name.** A public certificate authority
  issues it, and the device renews it through the account service, which
  holds the domain. Its private key stays on the device.
  - Whoever runs the domain could get a second certificate for the name
    and read along. The public certificate logs (Certificate
    Transparency) show every certificate issued. The device can compare
    them with its own, and an app that pins the device's key closes the
    gap.
  - This is weighed again when it is built.
- **Limits:**
  - pairing codes are short-lived and limited in tries (as SD-131);
  - remote sign-in attempts count against the same lockout as local ones;
  - the relay limits connections per device and per client.
- **CRA.** A server the product's functions depend on may count as part
  of the product under the Cyber Resilience Act. Then it gets the same
  care: vulnerability handling, security updates and a support period.
  How it counts is for the product session to settle with its legal
  advice.

## What drives running costs

No prices here; these are the drivers to weigh.

| Driver | Grows with |
|---|---|
| The relay's connections | Devices online, each holding one connection |
| The relay's traffic | How much owners use remote access |
| The account service | Owners and devices, certificate renewals; it is small |
| Backup storage | Devices, backups kept per device, backup size |
| Push | Messages; the platforms' push services cost little or nothing per message, a store app adds developer accounts and review cycles |
| Operation | Monitoring, security updates of the servers, support, for the whole support period of the product |

Remote access is the main variable cost, and a cap on it (for example a
fair-use limit) is a product question. The fixed costs are the servers'
operation and, with C, the store apps.

## Options for the product question

1. **Minimal.** Remote access through the relay, and encrypted backups.
   Push stays without an account (A). This has the smallest running costs
   and no store app.
2. **Minimal with web push** (1 with B). Push through the account, still
   without a store app.
3. **With a store app** (1 with C). This gives the most reliable push and
   an app in the stores, but more cost and work.
4. **None for now.** Remote access only through the owner's own VPN or
   Home Assistant, and the account later.

The software session recommends 1 first, with 2 as its next step. They
keep the frame (no subscription, minimal data) at the smallest running
cost. Push without an account already works (SD-124).

## Order of work once decided

1. The device security items needed: the device's key pair and
   certificate, and HTTPS per device.
2. The device side: the relay client, pairing and unpairing, the state of
   the connection, and the remote token.
3. The relay and the account service. Where their code lives (this
   repository or one of its own) and where they run is decided with the
   scope.
4. Encrypted backups.
5. Push, as decided (B or C).
