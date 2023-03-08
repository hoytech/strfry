const readline = require('readline');
const XorView = require('./xor.js');

const idSize = 16;

const rl = readline.createInterface({
  input: process.stdin,
  output: process.stdout,
  terminal: false
});

let n = 0;
let x1 = new XorView(idSize);
let x2 = new XorView(idSize);

rl.on('line', (line) => {
    let items = line.split(',');
    if (items.length !== 3) throw Error("too few items");

    let mode = parseInt(items[0]);
    let created = parseInt(items[1]);
    let id = fromHexString(items[2]);
    if (id.length !== idSize) throw Error("unexpected id size");

    if (mode === 1) {
        x1.addElem(created, id);
    } else if (mode === 2) {
        x2.addElem(created, id);
    } else if (mode === 3) {
        x1.addElem(created, id);
        x2.addElem(created, id);
    } else {
        throw Error("unexpected mode");
    }

    n++;
});

rl.once('close', () => {
    x1.finalise();
    x2.finalise();

    let q = x1.initial();

    while (q.length !== 0) {
        {
            let [newQ, haveIds, needIds] = x2.reconcile(q);
            q = newQ;

            for (let id of haveIds) console.log(`xor,2,HAVE,${id}`);
            for (let id of needIds) console.log(`xor,2,NEED,${id}`);
        }

        if (q.length !== 0) {
            let [newQ, haveIds, needIds] = x1.reconcile(q);
            q = newQ;

            for (let id of haveIds) console.log(`xor,1,HAVE,${id}`);
            for (let id of needIds) console.log(`xor,1,NEED,${id}`);
        }
    }
});




// FIXME: copied from xor.js

function fromHexString(hexString) {
    if ((hexString.length % 2) !== 0) throw Error("uneven length of hex string");
    return hexString.match(/../g).map((byte) => parseInt(byte, 16));
}

function toHexString(buf) {
    return buf.reduce((str, byte) => str + byte.toString(16).padStart(2, '0'), '');
}
