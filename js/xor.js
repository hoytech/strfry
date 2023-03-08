class XorView {
    constructor(idSize) {
        this.idSize = idSize;
        this.elems = [];
    }

    addElem(timestamp, id) {
        elems.push({ timestamp, id, });
    }

    finalise() {
        this.elems.sort(elemCompare);
        this.ready = true;
    }

    reconcile(query) {
        query = fromHexString(query);
        let output = [];
        let haveIds = [], needIds = [];

        let prevUpper = 0;
        let lastTimestampIn = 0;
        let lastTimestampOut = [0]; // wrapped in array so we can modify it by reference

        let decodeTimestampIn = () => {
            let timestamp = decodeVarInt(query);
            timestamp = timestamp === 0 ? Number.MAX_VALUE : timestamp - 1;
            if (lastTimestampIn === Number.MAX_VALUE || timestamp === Number.MAX_VALUE) {
                lastTimestampIn = Number.MAX_VALUE;
                return Number.MAX_VALUE;
            }
            timestamp += lastTimestampIn;
            lastTimestampIn = timestamp;
            return timestamp;
        };

        let decodeBoundKey = () => {
            let timestamp = decodeTimestampIn();
            let len = decodeVarInt(query);
            if (len > this.idSize) throw herr("bound key too long");
            let id = getBytes(query, len);
            return { timestamp, id, };
        };

        while (query.length !== 0) {
            let lowerBoundKey = decodeBoundKey();
            let upperBoundKey = decodeBoundKey();

            let lower = lowerBound(this.elems, prevUpper, this.elems.length, lowerBoundKey, elemCompare);
            let upper = upperBound(this.elems, lower, this.elems.length, upperBoundKey, elemCompare);
            prevUpper = upper;

            let mode = decodeVarInt(query);

            if (mode === 0) {
                let theirXorSet = getBytes(query, this.idSize);

                let ourXorSet = new Array(this.idSize).fill(0);
                for (let i = lower; i < upper; ++i) {
                    let elem = this.elems[i];
                    for (let j = 0; j < this.idSize; j++) ourXorSet[j] ^= elem[j];
                }

                let matches = true;
                for (let i = 0; i < this.idSize; i++) {
                    if (theirXorSet[i] !== ourXorSet[i]) {
                        matches = false;
                        break;
                    }
                }

                if (!matches) this._splitRange(lower, upper, lowerKey, upperKey, lastTimestampOut, output);
            } else if (mode >= 8) {
                let theirElems = {};
                for (let i = 0; i < mode - 8; i++) theirElems[getBytes(query, this.idSize)] = false;

                for (let it = lower; it < upper; it++) {
                    let e = theirElems[this.elems[i]];

                    if (e === undefined) {
                        // ID exists on our side, but not their side
                        haveIds.push(e.id);
                    } else {
                        // ID exists on both sides
                        theirElems[this.elems[i]] = true;
                    }
                }

                for (let k of Object.keys(theirElems)) {
                    if (!theirElems[k]) {
                        // ID exists on their side, but not our side
                        needIds.push(k);
                    }
                }
            } else {
                throw Error("unexpected mode");
            }
        }

        return [output, haveIds, needIds];
    }

    _splitRange(lower, upper, lowerKey, upperKey, lastTimestampOut, output) {
        let encodeTimestampOut = (timestamp) => {
            if (timestamp === Number.MAX_VALUE) {
                lastTimestampOut[0] = Number.MAX_VALUE;
                return encodeVarInt(0);
            }

            let temp = timestamp;
            timestamp -= lastTimestampOut[0];
            lastTimestampOut[0] = temp;
            return encodeVarInt(timestamp + 1);
        };

        let appendBoundKey = (key, output) => {
            output.push(...encodeTimestampOut(key.timestamp));
            output.push(...encodeVarInt(key.id.length));
            output.push(...key.id);
        };

        let appendMinimalBoundKey = (curr, prev) => {
            output.push(...encodeTimestampOut(curr.timestamp));

            if (curr.timestamp !== prev.timestamp) {
                output.push(...encodeVarInt(0));
            } else {
                let sharedPrefixBytes = 0;

                for (let i = 0; i < this.idSize; i++) {
                    if (curr.id[i] !== prev.id[i]) break;
                    sharedPrefixBytes++;
                }

                output.push(...encodeVarInt(sharedPrefixBytes + 1));
                output.push(...curr.id.slice(0, sharedPrefixBytes + 1));
            }
        };

        // Split our range
        let numElems = upper - lower;
        let buckets = 16;

        if (numElems < buckets * 2) {
            appendBoundKey(lowerKey);
            appendBoundKey(upperKey);

            output.push(...encodeVarInt(numElems + 8));
            for (let it = lower; it < upper; ++it) output.push(...this.elems[it].id);
        } else {
            let elemsPerBucket = Math.floor(numElems / buckets);
            let bucketsWithExtra = numElems % buckets;
            let curr = lower;

            for (let i = 0; i < buckets; i++) {
                if (i == 0) appendBoundKey(lowerKey);
                else appendMinimalBoundKey(this.elems[curr], this.elems[curr - 1]);

                let ourXorSet = new Array(this.idSize).fill(0);
                for (let bucketEnd = curr + elemsPerBucket + (i < bucketsWithExtra ? 1 : 0); curr != bucketEnd; curr++) {
                    for (let j = 0; j < this.idSize; j++) ourXorSet[j] ^= this.elems[curr][j];
                }

                if (i === buckets - 1) appendBoundKey(upperKey);
                else appendMinimalBoundKey(this.elems[curr], this.elems[curr - 1]);

                output.push(...encodeVarInt(0)); // mode = 0
                output.push(...ourXorSet.id);
            }
        }
    }
}






function fromHexString(hexString) {
    if ((hexString.length % 2) !== 0) throw Error("uneven length of hex string");
    return hexString.match(/../g).map((byte) => parseInt(byte, 16));
}

function toHexString(buf) {
    return buf.reduce((str, byte) => str + byte.toString(16).padStart(2, '0'), '');
}

function getByte(buf) {
    if (buf.length === 0) throw Error("parse ends prematurely");
    return buf.shift();
}

function getBytes(buf, n) {
    if (buf.length < n) throw Error("parse ends prematurely");
    return buf.splice(0, n);
}

function encodeVarInt(n) {
    if (n === 0) return [0];

    let o = [];

    while (n !== 0) {
        o.push(n & 0x7F);
        n >>>= 7;
    }

    o.reverse();

    for (let i = 0; i < o.length - 1; i++) o[i] |= 0x80;

    return o;
}

function decodeVarInt(buf) {
    let res = 0;

    while (1) {
        let byte = getByte(buf);
        res = (res << 7) | (byte & 127);
        if ((byte & 128) === 0) break;
    }

    return res;
}

function elemCompare(a, b) {
    if (a.timestamp === b.timestamp) {
        if (a.id < b.id) return -1;
        else if (a.id > b.id) return 1;
        return 0;
    }

    return a.timestamp - b.timestamp;
}

function binarySearch(arr, first, last, cmp) {
    let count = last - first;

    while (count > 0) {
        let it = first;
        let step = Math.floor(count / 2);
        it += step;

        if (cmp(arr[it])) {
            first = ++it;
            count -= step + 1;
        } else {
            count = step;
        }
    }

    return first;
}

function lowerBound(arr, first, last, value, cmp) {
    return binarySearch(arr, first, last, (a) => cmp(a, value) < 0);
}

function upperBound(arr, first, last, value, cmp) {
    return binarySearch(arr, first, last, (a) => cmp(value, a) >= 0);
}
