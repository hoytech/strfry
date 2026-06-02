# Community Integrations

These are third-party projects built around strfry. They are not maintained by the strfry authors.

If you know of other integrations worth listing here, please open a PR.

## Namecoin identity

* [strfry-namecoin-policy](https://github.com/mstrofnone/strfry-namecoin-policy) - write-policy plugin that verifies `.bit` [NIP-05](https://github.com/nostr-protocol/nips/blob/master/05.md) identities against the Namecoin blockchain via ElectrumX, so the relay only accepts `kind:0` events whose claimed `.bit` identity is backed on-chain.
* [strfry-nip05-namecoin](https://github.com/mstrofnone/strfry-nip05-namecoin) - HTTP sidecar that serves `/.well-known/nostr.json` from Namecoin `.bit` records, letting conventional NIP-05 clients verify `.bit`-backed identities with no Namecoin-specific support.
* [namecoin-nostr-bridge](https://github.com/mstrofnone/namecoin-nostr-bridge) - daemon that republishes Namecoin `d/*` identity records as Nostr events, turning the Namecoin chain into a Nostr-queryable identity index.
