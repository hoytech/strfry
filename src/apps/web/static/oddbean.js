const bech32Ctx = genBech32('bech32');

function encodeBech32(prefix, inp) {
    return bech32Ctx.encode(prefix, bech32Ctx.toWords(base16.decode(inp.toUpperCase())));
}

function decodeBech32(inp) {
    return base16.encode(bech32Ctx.fromWords(bech32Ctx.decode(inp).words)).toLowerCase();
}

document.addEventListener('alpine:init', () => {
    Alpine.data('obLogin', () => ({
        loggedIn: false,
        pubkey: '',
        username: '',

        init() {
            let storage = JSON.parse(window.localStorage.getItem('auth') || '{}');
            if (storage.pubkey) {
                this.loggedIn = true;
                this.pubkey = storage.pubkey;
                this.username = storage.username;
            }
        },

        async login() {
            let pubkey = await nostr.getPublicKey();

            let response = await fetch(`/u/${pubkey}/metadata.json`);
            let json = await response.json();
            console.log(json);

            let username = json.name;
            if (username === undefined) username = pubkey.substr(0, 8) + '...';
            if (username.length > 25) username = username.substr(0, 25) + '...';

            this.pubkey = pubkey;
            this.username = username;
            window.localStorage.setItem('auth', JSON.stringify({ pubkey, username, }));

            this.loggedIn = true;
        },

        myProfile() {
            return `/u/${encodeBech32('npub', this.pubkey)}`;
        },

        logout() {
            window.localStorage.setItem('auth', '{}');

            this.loggedIn = false;
        },
    }));

    Alpine.data('newPost', () => ({
        init() {
        },

        async submit() {
            this.$refs.msg.innerText = '';

            let ev = {
                created_at: Math.floor(((new Date()) - 0) / 1000),
                kind: 1,
                tags: [],
                content: this.$refs.post.value,
            };

            ev = await window.nostr.signEvent(ev);

            let resp = await fetch("/submit-post", {
                method: "post",
                headers: {
                    'Accept': 'application/json',
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(ev),
            });

            let json = await resp.json();

            if (json.message === 'ok' && json.written === true) {
                window.location = `/e/${json.event}`
            } else {
                this.$refs.msg.innerText = `Sending note failed: ${json.message}`;
                console.error(json);
            }
        },
    }));

    Alpine.data('newReply', (note) => ({
        async init() {
            let resp = await fetch(`/e/${note}/raw.json`);
            this.repliedTo = await resp.json();
        },

        async submit() {
            this.$refs.msg.innerText = '';

            let ev = {
                created_at: Math.floor(((new Date()) - 0) / 1000),
                kind: 1,
                tags: [],
                content: this.$refs.post.value,
            };

            {
                // e tags

                let rootId;
                for (let t of this.repliedTo.tags) {
                    if (t[0] === 'e' && t[3] === 'root') {
                        rootId = t[1];
                        break;
                    }
                }

                if (!rootId) {
                    for (let t of this.repliedTo.tags) {
                        if (t[0] === 'e') {
                            rootId = t[1];
                            break;
                        }
                    }
                }

                if (rootId) {
                    ev.tags.push(['e', rootId, '', 'root']);
                    ev.tags.push(['e', this.repliedTo.id, '', 'reply']);
                } else {
                    ev.tags.push(['e', this.repliedTo.id, '', 'root']);
                }

                // p tags

                let seenPTags = {};

                for (let t of this.repliedTo.tags) {
                    if (t[0] === 'p' && !seenPTags[t[1]]) {
                        ev.tags.push(['p', t[1]]);
                        seenPTags[t[1]] = true;
                    }
                }

                if (!seenPTags[this.repliedTo.pubkey]) {
                    ev.tags.push(['p', this.repliedTo.pubkey]);
                }

                // t tags

                ev.tags.push(['t', 'oddbean']);
            }

            ev = await window.nostr.signEvent(ev);

            let resp = await fetch("/submit-post", {
                method: "post",
                headers: {
                    'Accept': 'application/json',
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(ev),
            });

            let json = await resp.json();

            if (json.message === 'ok' && json.written === true) {
                window.location = `/e/${json.event}`
            } else {
                this.$refs.msg.innerText = `Sending note failed: ${json.message}`;
                console.error(json);
            }
        },
    }))
});


document.addEventListener("click", async (e) => {
    let parent = e.target.closest(".vote-context");
    if (!parent) return;

    let which = e.target.className;
    if (which.length !== 1) return;

    let note = parent.getAttribute('data-note');

    e.target.className = 'loading';
    e.target.innerText = '↻';

    if (which === 'f') return; // not impl


    try {
        let ev = {
            created_at: Math.floor(((new Date()) - 0) / 1000),
            kind: 7,
            tags: [],
            content: which === 'u' ? '+' : '-',
        };

        {
            let response = await fetch(`/e/${note}/raw.json`);
            let liked = await response.json();

            for (let tag of liked.tags) {
                if (tag.length >= 2 && (tag[0] === 'e' || tag[0] === 'p')) ev.tags.push(tag);
            }

            ev.tags.push(['e', liked.id]);
            ev.tags.push(['p', liked.pubkey]);
        }

        ev = await window.nostr.signEvent(ev);

        let response = await fetch("/submit-post", {
            method: "post",
            headers: {
                'Accept': 'application/json',
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(ev),
        });

        let json = await response.json();

        if (json.message === 'ok' && json.written === true) {
            e.target.className = 'success';
            e.target.innerText = '✔';
        } else {
            throw(Error(`Sending reaction note failed: ${json.message}`));
            console.error(json);
        }
    } catch(err) {
        console.error(err);
        e.target.className = 'error';
        e.target.innerText = '✘';
    }
});
