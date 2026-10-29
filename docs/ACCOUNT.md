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
  opened in the home network, and no port is forwarded. While remote
  access is on, the device's web server is still reachable from the
  internet, through the relay. So remote access is a switch of its own,
  off by default.
- **End to end.** The relay forwards encrypted bytes. It sees neither
  readings nor recipes nor passwords. Where this needs trust in the
  operator, the security notes say so.
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
  over TLS.
  - It authenticates with a key pair made on the device. The private key
    never leaves it and is kept with the device security items of #10.
  - It reconnects with growing waits and some randomness, so devices do
    not all return at once after an outage.
  - Which relay it uses is a setting. Owners can run their own relay if
    the service ever ends.
- **The relay** joins a client's connection to its device's connection
  and forwards bytes.
  - It knows a device by its public key.
  - A client names the device by a host name under the relay's domain.
    The name is made from a hash of the device's public key, not from the
    device ID: the ID is public anyway (`/info`, MQTT, diagnostics), and a
    name from the key cannot be taken by another device.
  - The relay routes by the name in the TLS handshake (SNI) and does not
    end TLS. It holds no keys for the content.
  - It limits connections and handshakes per device and per source
    address.
- **The account service** signs the owner in to the account (not to the
  device) and keeps:
  - the owner's devices, by device ID and public key;
  - the encrypted backups;
  - where present, push subscriptions of its own.
- **The client** is a browser, or later an app.
  - Its TLS ends on the device, which holds a certificate for its host
    name. So the web app a browser loads comes from the device itself, as
    in the home network, and not from a server.
  - An app can also pin the device's key from the pairing.
  - The relay is only the cable.

### Pairing a device with an account

1. In the local web app, signed in as the owner, the owner asks for a
   pairing code. It is short-lived and shown once, as a QR code, like the
   coupling code of SD-131. The code is bound to the device's ID and its
   key's fingerprint, so the service matches it to one device only.
2. The owner enters or scans it in the account. The device shows the
   account that claims it, and the owner confirms it there. Only then
   does the service list the device and the relay route its name.
3. Tries are limited per device, per account and in total.
4. Remote access stays off until the owner switches it on in the local
   web app. Pairing alone opens nothing.

### Signing in from afar

- **No password from afar.** The device's password never goes through
  the relay. A remote sign-in uses a passkey for the device's host name.
  - The owner adds one in the local web app. A one-time code, shown once,
    lets a phone or browser register its passkey through the relay.
  - Each passkey is listed with its name and last use, and the owner
    removes each on its own.
- **At most the account role.** A call through the relay gets at most the
  `account` role of SD-129. The one exception is an app that pins the
  device's key from the pairing: its access token (SD-129) keeps its
  role.
  - With the `account` role, a remote client can read, use Stop,
    acknowledge alarms, and change recipes and plants.
  - It never reaches limits, releases, calibration, single pumps,
    settings, the backup or maintenance. These stay at home, or with an
    app that pins the device's key.
- **Its own count.** Failed remote sign-ins have a count and a lockout of
  their own. They never lock local sign-in. Every remote sign-in is an
  event.
- **The servers call nothing.** The relay and the account service hold
  no password, passkey or token. They can call nothing on the device.
- **Unpairing.**
  - At the device, unpairing ends every session from afar at once and
    removes the relay registration.
  - In the account, the service removes the listing, and the relay stops
    routing that key.
  - Either way, the device reports it and goes on locally.
- **Factory reset.** A reset makes a new key pair, and with it a new
  host name. A device that is passed on cannot be reached under its old
  name.

### Push through the account

There are three ways, from least to most to build and run:

- **A. None of its own.** Push stays with ntfy, Telegram, e-mail and Home
  Assistant (SD-124, SD-130). The account adds nothing here.
- **B. Web push.** The device's web app is installed on the phone from
  the browser, and the browser's push service delivers a message.
  - The message is encrypted for that subscription (RFC 8291), so the
    push service sees only ciphertext.
  - It needs an HTTPS origin, and the device's host name is one.
  - The device keeps the subscription and sends the push itself, signed
    with a key of its own (RFC 8292). That key never leaves the device.
  - A push that the device cannot send, such as "device not reachable",
    needs a second subscription on the account's own page.
  - The device accepts a subscription only for HTTPS endpoints of the
    known push services. So it cannot be made to call into the home
    network.
  - It needs no store app. Each platform and browser decides when web
    push is shown; this is to be checked when it is built.
- **C. A store app.** A native app receives push through the platforms'
  push services. This works most reliably, but needs an app in each store,
  with its review and developer accounts.

For B and C the alarm text can stay on the device. The push then says
only "new alarm on <device name>", and the client fetches the details
through the relay.

### Backup

- **Encrypted on the device.** The device encrypts a backup (as
  `GET /backup` makes it) before it is uploaded.
  - The key comes from a passphrase the owner chooses, through a
    memory-hard key derivation as strong as the device allows. Its salt
    is in the backup's header.
  - The encryption is authenticated. It covers the device ID and the
    backup's time, so the service can neither swap backups between
    devices nor pass an old one off as new.
- **Kept on the server.** The account service keeps the last few backups
  per device. It cannot read them; a forgotten passphrase is a lost
  backup.
- **What is left out.** As SD-129 says, a backup never carries the device
  ID, the access tokens or device secrets. The device ID that the
  encryption covers sits in the file's header, outside the backup
  itself.

### What the account keeps

- The owner's sign-in to the account (e-mail with a passkey or a
  password; to be chosen when built).
- Per device: its ID, its public key (the host name comes from it), a
  name the owner gives, when it was last connected.
- Encrypted backups, and push subscriptions of its own where B or C is
  chosen.

Never: readings, recipes, plants, events, passwords, passkeys, tokens,
or anything else in clear.

The relay and the account service also see the IP addresses of homes and
clients, and when they connect. They keep them only as long as operation
needs. How long is set when built, together with the privacy notice.

### When the server is down

- **The device:** it goes on alone, as without an account.
- **The relay connection:** it is retried, and the state says so.
- **Without the server:** remote access, account push and backup uploads
  stop. Local access, push without an account, MQTT and the local backup
  go on.

## Security notes

- **The main surface.** Remote access is the largest attack surface the
  product will have.
  - It is off until the owner switches it on.
  - The device enforces the roles itself, and caps the streams and
    handshakes it takes through the relay.
  - The host names are public: certificates are logged, and the relay
    shows whether a device is online.
- **Who must be trusted (browser).** A browser trusts any certificate
  from a public authority. Whoever runs the relay or the domain's DNS
  could get a second certificate for a device's name, sit between browser
  and device, and read and act along.
  - In a browser, end to end therefore rests on trust in the operator.
    This is detected, not prevented: the public certificate logs
    (Certificate Transparency) show every certificate issued. The device
    checks them for its name and raises an alarm on one it did not ask
    for.
  - The damage stays small. No password goes through the relay, so
    nothing caught there opens the device at home. A remote session gets
    at most the `account` role.
  - An app that pins the device's key closes the gap.
- **Certificates.**
  - The device holds its own account with a public certificate
    authority (ACME). It proves its name itself through the relay's
    route (TLS-ALPN-01). The private key stays on the device.
  - The device zone goes on the Public Suffix List. Then each device's
    name counts as a site of its own: one device cannot set cookies for
    another, and the certificate authority's limits count per device, not
    for the whole zone.
  - Certificates are short-lived. Browsers barely check revocation, so a
    device is cut off by the relay refusing its key.
- **Sessions.** Over HTTPS the session cookie is a `__Host-` cookie with
  `Secure`, and same-origin checks stay.
- **Keys.** The device's key pair and HTTPS per device belong to the
  device security items of #10, with signed updates, encrypted storage,
  and factory and access reset. They are a precondition of the device
  side.
- **CRA, EN 18031 and privacy.** A server the product's functions depend
  on may count as part of the product under the Cyber Resilience Act.
  Then it gets the same care: vulnerability handling, security updates
  and a support period. Remote access off by default and remote sign-ins
  as events serve its secure defaults and logging. These are for the
  product session to settle with its legal advice:
  - how the server counts;
  - whether EN 18031-2 applies;
  - the data protection of the addresses above.

## What drives running costs

No prices here; these are the drivers to weigh.

| Driver | Grows with |
|---|---|
| The relay's connections | Devices online, each holding one connection |
| The relay's traffic | How much owners use remote access |
| The account service | Owners and devices; it is small |
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
   an app in the stores. It is also the only client that pins the
   device's key, so not even the operator can read along. It costs more
   money and work.
4. **None for now.** Remote access only through the owner's own VPN or
   Home Assistant, and the account later.

The software session recommends 1 first, with 2 as its next step. They
keep the frame (no subscription, minimal data) at the smallest running
cost. Push without an account already works (SD-124).

## Order of work once decided

1. The device security items needed: the device's key pair, and HTTPS
   per device.
2. The device side:
   - the relay client;
   - pairing and unpairing;
   - the switch for remote access;
   - remote sign-in with passkeys and its own count;
   - the certificate through ACME;
   - the check of the certificate logs;
   - the state of the connection.
3. The relay and the account service, and the device zone on the Public
   Suffix List. Where their code lives (this repository or one of its
   own) and where they run is decided with the scope.
4. Encrypted backups.
5. Push, as decided (B or C).
