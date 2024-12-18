# Nostr Event Feeds

Feeds are for publishing curated lists of nostr events, (like an RSS feed). Feeds are intended to be chronological, in other words new items are pushed onto the end of the list, and are typically displayed in reverse order (most recent first).

Feeds are referenced by a curator's pubkey and a feed name. The end of the list is stored in an event called the "head event". The head references previous "continuation events", forming a [singly-linked list](https://en.wikipedia.org/wiki/Linked_list#Singly_linked_list).

Both head events and continuation events refer to their items using standard nostr `e` tags. Head events should also include information about what the feed is, its editorial policy, etc.

Head events are replaceable, but continuation events are not. In order to edit history, a new sequence of continuation events should be created, back to the alteration. However, the original sequence of continuation events will continue to exist (it is immutable). Feeds can be forked since multiple head events can reference the same sequence of continuation events.

## Head Events

The head event is updated every time a new event is added to the feed. They use kind `33800`, which is a parameterised replaceable event. The `d` tag should be the feed name, allowing a pubkey to publish multiple feeds.

Normal `e` tags refer to `id`s of other nostr events, selected by the curator. The first `e` tag is interpreted to be at the end-most position (most recent).

If there is a chain of continuation events, a `prev` tag references the next event's `id`.

The `content` field should contain a JSON-encoded object with feed meta-data. The fields are unspecified, but should include at least `title` and `description`.

### Example:

    [
      {
        "kind": 33800,
        "created_at": 1734475602,
        "tags": [
          ["d", "my-cool-feed"],
          ["prev", "f26aae2b1a339c0c3e4dbe4e024b150481aec2b2448f4459929ac13f5b695758"],
          ["e", "801627607c87e48ee2df888b0a006df0bd723b4ab40d7837956e1eefbe5aba5b"],
          ["e", "586716c7f324777024c2028fb62e8efddbdc4483facf41adb205742b3ec7e740"],
          ["e", "782d4710fdbe59cac25126dea3dbc5e957ecff2aa094c70b43c61c6e566a5dde"],
        ],
        "content": "{\"title\":\"My Cool Feed\",\"description\":\"List of cool events.\"}",
        "pubkey": "56785430491e4bdf85b6a46e2a08e8d8a29eff0ed2b84aeb6406d2bea5ad4bf1",
        "id": "634b5ab623113283a47f403039d420a7bc604fa97df8bbbac1b2f8df0ecf89a8",
        "sig": "b45bea25f09f3e2a721372abae24592515ffd533029fc3c891ad3645ccd2c428effd39c317f5e24fff7f178701ef38c08d230c09d5044ee1ae52d0a39edf788a"
      }
    ]

## Continuation Events

When a head event gets too big, a continuation event should be created, and a new head event should refer to it. When an event gets "too-big" is up to the feed creator, although 50 `e` tags is a reasonable starting-point.

Continuation events use kind `43800`, which is a regular (non-replaceable) event. Instead of using `d` for the feed name, the feed name should be stored in `feed`. The `content` field should simply be empty.

### Example

    [
      {
        "kind": 43800,
        "created_at": 1734389202,
        "tags": [
          ["feed", "my-cool-feed"],
          ["prev", "70e8492955da1a73961b3ff176264f1cb75ff5ec4826b4365218b38804740aa5"],
          ["e", "30fdf8b5ef27cc7ae6879878136e1a715025ed0ad7a72c752a8e2c08bd7dd470"],
          ["e", "3791691e283ac6ed29ab77c3403a22c3c5523192f641bfff300f224fab42b9c1"],
          ["e", "d419933e98de6187c8263f86319b9fd767f40143200c0752764da4159dd4cb47"],
        ],
        "content": "",
        "pubkey": "56785430491e4bdf85b6a46e2a08e8d8a29eff0ed2b84aeb6406d2bea5ad4bf1",
        "id": "d447e1fa611f657513a1db61bd2da360c30e6f5ad464df952a0daa9e2f695e6e",
        "sig": "93b96dca9e118f859b454874683519408cc291a6aa6ad2a04209a577ca18c9318b629449f522164f23dcc5473a5a97f1c2b594c2f10016041ca678e822f67443"
      }
    ]
