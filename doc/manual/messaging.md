+++
id = "messaging"
title = "Messaging, contacts, and credential security"
section = "service"
summary = "Manage contacts, conversations, trust, and saved credentials"
aliases = ["contacts.service", "credentials"]
keywords = "messaging contacts credentials trust endpoint gateway meshcore security"
packages_any = ["service_contacts", "service_credentials"]
+++
# Messaging identities and security

Run `messages` without arguments to open the tabbed instance, connection, and
message manager. It supports provider settings forms, connection diagnostics,
conversation and unread browsing, pending-send cancellation, and deletion of
individual local messages. `c` opens the existing Chat as a child and returns
to the browser when Chat closes. See [Application controls](apps.md#messages).

To start a direct conversation by name, open `contacts`, select the person,
and press `c`. Chat opens their conversation and returns to Contacts when
closed. The Chat application must be included in the firmware.

Provider classes describe protocols; named instances describe individual
connections. Multiple IRC instances can share a class while keeping server
settings, rooms, nicknames, and message routing separate. Chat groups by
instance and accepts `chat INSTANCE`, `chat CLASS`, or a conversation ID.
Use `/join INSTANCE TARGET`, `/query INSTANCE NICK`, and `/leave` in Chat,
or `messages join|query INSTANCE TARGET` and `messages leave CONVERSATION_ID`
from the shell. See [Native IRC](messaging.irc.md).

`messages remove INSTANCE` (name or ID), or `/remove INSTANCE` in Chat, removes
a configurable provider instance. It closes the connection, stops retries,
cancels queued sends, and removes the saved connection settings and credentials.
Local messages, conversations, and associated Inbox notifications are deleted.
Gateway, MeshCore, and Link support removing their default instances too.
Removal requires writable mounted storage.

`messages classes` lists provider classes; `messages instances` lists configured
connections. `messages add gateway NAME URL [TOKEN]` creates a Gateway connection.
`messages add link NAME TRANSPORT` creates a Link projection. Enable it with
`messages connect NAME`, and run the corresponding transport job. Chat accepts
the same arguments after `/add`, and changes settings with `/configure NAME ...`.
MeshCore settings are `messages add meshcore NAME radio RADIO PROFILE` or
`messages add meshcore NAME ble ADDRESS TYPE [PIN]`; see [MeshCore](meshcore.md).

| Class | Settings after `messages add CLASS NAME` | Worker |
| --- | --- | --- |
| `gateway` | `URL [TOKEN]` | `gateway-sync` |
| `irc` | `HOST PORT NICK` | `irc-sync` |
| `meshcore` | `radio RADIO PROFILE` or `ble ADDRESS TYPE [PIN]` | `meshcore` or `meshcore-ble` |
| `link` | `TRANSPORT` | Its `radio-link` or `espnow-link` transport with `chat=on` |

New instances start disabled. Use `messages connect NAME` and start the worker.
`messages configure NAME SETTINGS...` changes connection settings; IRC also
accepts `sasl ACCOUNT PASSWORD`, `sasl off`, `pass PASSWORD|off`, and `ca FILE|default`.
Chat exposes `/classes`, `/instances`, `/add CLASS NAME SETTINGS...`,
`/configure NAME SETTINGS...`, and `/remove NAME`. Python and Lua expose the same
class registry and instance operations through `solaros.messages`.

A small record containing only the retired instance ID, name, and class stays
on mounted storage. Retired IDs are never reused, so contacts and retained
messages cannot be routed through a different account. Creating the same name
again creates an independent instance. `messages clear INSTANCE_ID` deletes
history without removing an active instance. Interrupted removals finish deleting
history on the next boot.

SolarOS separates messaging identity from transport. A contact is one local
address-book entry, while each gateway, MeshCore, or SolarOS Link address is an
endpoint with its own trust and capabilities. Linking endpoints does not copy
trust between providers.

Trust has three states:

- `discovered`: a provider supplied a valid address or advert;
- `trusted`: the user explicitly accepts that endpoint identity;
- `blocked`: direct inbound and outbound messaging is denied.

A signed MeshCore advert proves ownership of its public key, not the human
identity using the device. New adverts therefore remain `discovered` until the
user trusts them.

Contacts can retain up to 512 contacts and 576 endpoints. They are saved in
`/.contacts/contacts.bin` (under 192 KiB). If storage is unavailable, contacts
remain usable for the current boot; check `contacts status` for save errors.
Imports keep existing contacts and report skipped entries when full.

The credential store holds provider keys and tokens. Secret values are not
shown in Inbox, logs, autocomplete, agent tools, Python, or Lua. They are saved in
`.settings/credentials.bin` on mounted storage. Existing NVS records migrate only
after a checked file is committed. The file is not encrypted. SSH keys, agent API keys, and other provider tokens keep their separate
stores.

See `contacts` for the TUI and mutation commands. `chat` is the unified
conversation UI and works without Wi-Fi; provider jobs connect transports when
available. The `messages` shell API exposes the same disk history.
Stable 64-bit message IDs are printed as hexadecimal strings so they
survive shell and scripting runtimes without precision loss.

Received and sent messages are saved in `.messages/history.bin` on the selected
storage volume. The 64-message RAM cache does not limit disk history. History
grows with available disk space; gateway sync and cache eviction do not delete
older messages. Chat loads each conversation from disk. Use Page Up and Page
Down to move through older and newer history pages. `messages list ID` reads
the complete saved conversation.

SolarOS selects mounted storage when creating the archive and saves that
location in internal flash. If SD mounts after startup, an internal archive
moves to SD through a checked, synced copy before the saved location changes.
Existing files on SD are not overwritten. An unavailable saved volume is retried
when it returns; a new empty archive does not replace its history.
If storage is unavailable or a write fails, `messages status` reports the error.
New sends are queued only after their history is saved.

Existing full message records and incoming message records from the compact
Inbox store migrate to the archive. Their original files are retained.

Direct messages to a blocked endpoint are always rejected. A discovered
endpoint requires an explicit second confirmation in Chat or
`--allow-untrusted` in the shell. That opt-in applies only to the one send and
does not change the endpoint's trust state.

MeshCore group labels distinguish `[radio]` from `[companion]`. A retained
group outside the current transport or channel configuration has a `(history)`
label: its messages remain readable, but it cannot accept new sends. Outbound
queues are held in memory; after a restart, retained `queued` or `sending`
messages become `failed` and must be resent explicitly.

## Quick reference

```text
contacts
contacts status
contacts list [all|discovered|trusted|blocked]
contacts show CONTACT_ID
contacts rename CONTACT_ID NAME
contacts trust CONTACT_ID [ENDPOINT_ID]
contacts block CONTACT_ID [ENDPOINT_ID]
contacts remove CONTACT_ID
contacts link TARGET_CONTACT_ID SOURCE_CONTACT_ID
messages
messages status
messages conversations
messages list CONVERSATION_ID
messages send CONVERSATION_ID TEXT [--allow-untrusted]
messages read CONVERSATION_ID
messages delete MESSAGE_ID
messages remove INSTANCE
messages clear INSTANCE|INSTANCE_ID|all
messages outbox
messages cancel MESSAGE_ID
outbox [list]
outbox cancel MESSAGE_ID
```

Conversations hold message history. Inbox shows incoming notifications from
messaging and other apps. Outbox holds pending sends.

`messages clear` also removes the selected provider's Inbox projections,
including stale projections whose retained ring slot has already wrapped. It
does not remove unrelated Inbox sources; use `inbox clear` for the entire
universal Inbox.

For packet-radio operation, see `meshcore` and `link`. MeshCore direct messages
use the same conversations and trust states as gateway messages; shared-key
groups remain visibly sender-unverified. A `radio-link` or `espnow-link` job
started with `chat=on` adds unencrypted, packet-sized Link broadcast and direct
conversations and discovers source device IDs as Contacts.
