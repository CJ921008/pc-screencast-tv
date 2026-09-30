# LAN ScreenCast protocol

Milestone 0 establishes the signaling envelope only. Discovery, signaling transport, pairing and media are implemented in later milestones.

`signaling.schema.json` uses JSON Schema Draft 2020-12. Each message has `type`, `requestId`, `timestamp` (Unix milliseconds), and `payload`; protocol version is 1. Message-specific payload validation will be added when signaling is implemented.

