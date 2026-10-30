/* Copyright (C) 2026 low trust society */
/*
 * XDJ-700 PLUS patcher, the part that does the work, for the web page
 * (tools/web_patcher_template.html; tools/make_patcher.py puts both into
 * patcher/xdj700_plus_patcher.html with the mod's data).
 *
 * It does in the browser, on the user's own computer, what the Python
 * patcher (tools/patcher_template.py) does, step for step and with the same
 * checks: it takes the official 1.15 update file (or the zip it comes in),
 * puts the mod's bytes into the unpacked application, packs it again,
 * writes the update file and compares it with the tested build's SHA-256.
 * Nothing is sent anywhere. It also makes the zip the page hands out and
 * checks a file the user copied to the stick.
 *
 * Plain JavaScript, no libraries. Runs in a browser and, for the tests
 * (tools/web_patcher_check.js), in Node.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
(function (root) {
    "use strict";

    var APPLICATION_OFFSET = 0x40000;
    var MAIN_IMAGE_BYTES = 7251904;
    var APPLICATION_SPAN_BYTES = MAIN_IMAGE_BYTES - APPLICATION_OFFSET;
    var FLASH_LIMIT = 0x7E0000;
    var BOOT_REGION_LINES = 4081;
    var RECORD_BYTES = 32;
    var WINDOW = 4096;
    var MAX_MATCH = 18;
    var STOCK_TAG = ascii("MAINVer1.15\0");
    var ADDRESS_BYTES = { "0": 2, "1": 2, "2": 3, "3": 4, "5": 2, "6": 3, "7": 4, "8": 3, "9": 2 };

    function PatchError(message) {
        this.name = "PatchError";
        this.message = message;
    }
    PatchError.prototype = Object.create(Error.prototype);

    /* Errors that can only come from our side, as in the Python patcher: each is raised only once the input is the official file. */
    var OURS = ["the packed application is damaged", "the packed application's checksum is wrong",
        "the unpacked application is not the expected one", "the version tag was not found", "internal error:"];
    var ISSUES = "https://github.com/lowtrustsociety/XDJ-700-PLUS/issues";
    var REPORT = "this should not happen with the official file: please report it at";

    function isOurs(error) {
        return OURS.some(function (start) { return error.message.indexOf(start) === 0; });
    }

    function ascii(text) {
        var out = new Uint8Array(text.length);
        for (var i = 0; i < text.length; i++) out[i] = text.charCodeAt(i);
        return out;
    }

    function concat(parts) {
        var length = 0;
        parts.forEach(function (part) { length += part.length; });
        var out = new Uint8Array(length);
        var at = 0;
        parts.forEach(function (part) { out.set(part, at); at += part.length; });
        return out;
    }

    function equal(a, b) {
        if (a.length !== b.length) return false;
        for (var i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
        return true;
    }

    function tick() {
        return new Promise(function (resolve) { setTimeout(resolve, 0); });
    }

    /* ---- SHA-256: the browser's own where it may be used, otherwise this one ---- */

    var K = new Uint32Array([
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    ]);

    function sha256Plain(data) {
        var h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
        var w = new Uint32Array(64);
        var total = data.length;
        var tailLength = ((total + 9 + 63) >> 6 << 6) - total;
        var tail = new Uint8Array(tailLength);
        tail[0] = 0x80;
        var bits = total * 8;
        for (var b = 0; b < 8; b++) {
            tail[tailLength - 1 - b] = Math.floor(bits / Math.pow(2, 8 * b)) & 0xFF;
        }
        function block(bytes, at) {
            for (var i = 0; i < 16; i++) {
                w[i] = (bytes[at + 4 * i] << 24) | (bytes[at + 4 * i + 1] << 16) | (bytes[at + 4 * i + 2] << 8) | bytes[at + 4 * i + 3];
            }
            for (i = 16; i < 64; i++) {
                var x = w[i - 15], y = w[i - 2];
                var s0 = ((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3);
                var s1 = ((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10);
                w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
            }
            var a = h[0], b2 = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
            for (i = 0; i < 64; i++) {
                var S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
                var t1 = (k + S1 + ((e & f) ^ (~e & g)) + K[i] + w[i]) | 0;
                var S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
                var t2 = (S0 + ((a & b2) ^ (a & c) ^ (b2 & c))) | 0;
                k = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b2; b2 = a; a = (t1 + t2) | 0;
            }
            h[0] += a; h[1] += b2; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
        }
        var whole = total - (total % 64);
        for (var at = 0; at < whole; at += 64) block(data, at);
        var last = concat([data.subarray(whole), tail]);
        for (at = 0; at < last.length; at += 64) block(last, at);
        var out = "";
        for (var i = 0; i < 8; i++) out += ("00000000" + h[i].toString(16)).slice(-8);
        return out;
    }

    function sha256(data) {
        var subtle = root.crypto && root.crypto.subtle;
        if (!subtle) return Promise.resolve(sha256Plain(data));
        return subtle.digest("SHA-256", data).then(function (digest) {
            return Array.prototype.map.call(new Uint8Array(digest), function (v) {
                return ("0" + v.toString(16)).slice(-2);
            }).join("");
        }, function () {
            return sha256Plain(data);
        });
    }

    /* ---- Checksums ---- */

    var CRC16_TABLE = (function () {
        var table = new Uint16Array(256);
        for (var n = 0; n < 256; n++) {
            var c = n << 8;
            for (var k = 0; k < 8; k++) c = (c & 0x8000) ? ((c << 1) ^ 0x1021) : (c << 1);
            table[n] = c & 0xFFFF;
        }
        return table;
    })();

    /* CRC-16/XMODEM, as Python's binascii.crc_hqx(data, 0). */
    function crcHqx(data) {
        var crc = 0;
        for (var i = 0; i < data.length; i++) crc = ((crc << 8) & 0xFF00) ^ CRC16_TABLE[((crc >> 8) ^ data[i]) & 0xFF];
        return crc;
    }

    var CRC32_TABLE = (function () {
        var table = new Uint32Array(256);
        for (var n = 0; n < 256; n++) {
            var c = n;
            for (var k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
            table[n] = c >>> 0;
        }
        return table;
    })();

    function crc32(data) {
        var crc = 0xFFFFFFFF;
        for (var i = 0; i < data.length; i++) crc = CRC32_TABLE[(crc ^ data[i]) & 0xFF] ^ (crc >>> 8);
        return (crc ^ 0xFFFFFFFF) >>> 0;
    }

    function sum16(data, start, end) {
        var total = 0;
        for (var i = start; i < end; i++) total = (total + data[i]) & 0xFFFF;
        return total;
    }

    /* ---- Streams the browser unpacks (zlib, and raw deflate inside zip files) ---- */

    function inflate(data, format) {
        if (typeof root.DecompressionStream !== "function") {
            return Promise.reject(new PatchError("this browser is too old to unpack the files: use a current Chrome, Edge, Firefox or Safari"));
        }
        var stream = new Blob([data]).stream().pipeThrough(new root.DecompressionStream(format));
        return new Response(stream).arrayBuffer().then(function (buffer) { return new Uint8Array(buffer); });
    }

    function fromBase64(text) {
        if (typeof root.atob === "function") {
            var binary = root.atob(text);
            var out = new Uint8Array(binary.length);
            for (var i = 0; i < binary.length; i++) out[i] = binary.charCodeAt(i);
            return out;
        }
        return new Uint8Array(Buffer.from(text, "base64"));
    }

    /* ---- Zip files: reading Pioneer's download, writing ours ---- */

    function u16(data, at) { return data[at] | (data[at + 1] << 8); }
    function u32(data, at) { return (data[at] | (data[at + 1] << 8) | (data[at + 2] << 16) | (data[at + 3] << 24)) >>> 0; }

    function isZip(data) {
        return data.length >= 4 && data[0] === 0x50 && data[1] === 0x4B;
    }

    /* The entries of a zip file: name, method, sizes, CRC-32 and where the data starts. */
    function zipEntries(data) {
        var end = -1;
        for (var at = data.length - 22; at >= Math.max(0, data.length - 22 - 65535); at--) {
            if (u32(data, at) === 0x06054B50) { end = at; break; }
        }
        if (end < 0) throw new PatchError("this zip file cannot be read");
        var count = u16(data, end + 10);
        var position = u32(data, end + 16);
        var entries = [];
        for (var n = 0; n < count; n++) {
            if (position + 46 > data.length || u32(data, position) !== 0x02014B50) throw new PatchError("this zip file cannot be read");
            var nameLength = u16(data, position + 28);
            var extraLength = u16(data, position + 30);
            var commentLength = u16(data, position + 32);
            var local = u32(data, position + 42);
            var name = "";
            for (var i = 0; i < nameLength; i++) name += String.fromCharCode(data[position + 46 + i]);
            if (local + 30 > data.length || u32(data, local) !== 0x04034B50) throw new PatchError("this zip file cannot be read");
            entries.push({
                name: name,
                method: u16(data, position + 10),
                crc: u32(data, position + 16),
                packedSize: u32(data, position + 20),
                size: u32(data, position + 24),
                start: local + 30 + u16(data, local + 26) + u16(data, local + 28)
            });
            position += 46 + nameLength + extraLength + commentLength;
        }
        return entries;
    }

    function zipRead(data, entry) {
        var packed = data.subarray(entry.start, entry.start + entry.packedSize);
        if (packed.length !== entry.packedSize) return Promise.reject(new PatchError("this zip file cannot be read"));
        var unpacked;
        if (entry.method === 0) unpacked = Promise.resolve(packed);
        else if (entry.method === 8) unpacked = inflate(packed, "deflate-raw");
        else return Promise.reject(new PatchError("this zip file cannot be read"));
        return unpacked.then(function (content) {
            if (content.length !== entry.size || crc32(content) !== entry.crc) throw new PatchError("this zip file is damaged");
            return content;
        }, function (error) {
            throw error instanceof PatchError ? error : new PatchError("this zip file cannot be read");
        });
    }

    /* A zip file of the given {name, data} entries, stored as they are (no packing), each with its CRC-32. */
    function zipWrite(files, date) {
        date = date || new Date();
        var time = (date.getHours() << 11) | (date.getMinutes() << 5) | (date.getSeconds() >> 1);
        var day = ((date.getFullYear() - 1980) << 9) | ((date.getMonth() + 1) << 5) | date.getDate();
        var locals = [];
        var centrals = [];
        var offset = 0;
        files.forEach(function (file) {
            var name = ascii(file.name);
            var crc = crc32(file.data);
            var header = new Uint8Array(30 + name.length);
            var view = new DataView(header.buffer);
            view.setUint32(0, 0x04034B50, true);
            view.setUint16(4, 20, true);
            view.setUint16(8, 0, true);
            view.setUint16(10, time, true);
            view.setUint16(12, day, true);
            view.setUint32(14, crc, true);
            view.setUint32(18, file.data.length, true);
            view.setUint32(22, file.data.length, true);
            view.setUint16(26, name.length, true);
            header.set(name, 30);
            var central = new Uint8Array(46 + name.length);
            view = new DataView(central.buffer);
            view.setUint32(0, 0x02014B50, true);
            view.setUint16(4, 20, true);
            view.setUint16(6, 20, true);
            view.setUint16(12, time, true);
            view.setUint16(14, day, true);
            view.setUint32(16, crc, true);
            view.setUint32(20, file.data.length, true);
            view.setUint32(24, file.data.length, true);
            view.setUint16(28, name.length, true);
            view.setUint32(42, offset, true);
            central.set(name, 46);
            locals.push(header, file.data);
            centrals.push(central);
            offset += header.length + file.data.length;
        });
        var directory = concat(centrals);
        var end = new Uint8Array(22);
        var view = new DataView(end.buffer);
        view.setUint32(0, 0x06054B50, true);
        view.setUint16(8, files.length, true);
        view.setUint16(10, files.length, true);
        view.setUint32(12, directory.length, true);
        view.setUint32(16, offset, true);
        return concat(locals.concat([directory, end]));
    }

    /* The update file's bytes, from the file itself or from the zip Pioneer ships it in. */
    function readOfficialFile(data) {
        if (!isZip(data)) return Promise.resolve(data);
        var entries = zipEntries(data).filter(function (entry) {
            return entry.name.toUpperCase().slice(-10) === "XDJ700.UPD";
        });
        if (entries.length !== 1) return Promise.reject(new PatchError("no XDJ700.UPD in this zip file"));
        return zipRead(data, entries[0]);
    }

    /* ---- The update file ---- */

    /* The update file: two length lines, then the MAIN and the PANEL component. */
    function splitContainer(data) {
        var first = -1, second = -1;
        for (var i = 0; i + 1 < data.length && i < 64; i++) {
            if (data[i] === 13 && data[i + 1] === 10) {
                if (first < 0) first = i;
                else { second = i; break; }
            }
        }
        var digits = /^[0-9]+$/;
        var a = first < 0 ? "" : String.fromCharCode.apply(null, data.subarray(0, first));
        var b = second < 0 ? "" : String.fromCharCode.apply(null, data.subarray(first + 2, second));
        if (!digits.test(a) || !digits.test(b)) throw new PatchError("this is not an XDJ-700 update file");
        var lengths = [parseInt(a, 10), parseInt(b, 10)];
        var offset = second + 2;
        if (offset + lengths[0] + lengths[1] !== data.length) throw new PatchError("the update file is damaged (lengths do not add up)");
        return { lengths: lengths, offset: offset };
    }

    /* Where each line starts and ends (its line break included), as Python's splitlines(keepends=True). */
    function lineBounds(text) {
        var starts = [];
        var ends = [];
        var start = 0;
        for (var i = 0; i < text.length; i++) {
            var c = text[i];
            if (c === 10 || c === 13) {
                var end = (c === 13 && text[i + 1] === 10) ? i + 2 : i + 1;
                starts.push(start);
                ends.push(end);
                start = end;
                i = end - 1;
            }
        }
        if (start < text.length) {
            starts.push(start);
            ends.push(text.length);
        }
        return { starts: starts, ends: ends };
    }

    function hexValue(c) {
        if (c >= 48 && c <= 57) return c - 48;
        if (c >= 65 && c <= 70) return c - 55;
        if (c >= 97 && c <= 102) return c - 87;
        return -1;
    }

    /* The flash image a block of Motorola S-records describes. */
    function decodeRecords(text) {
        var image = new Uint8Array(FLASH_LIMIT);
        image.fill(0xFF);
        var end = 0;
        var bounds = lineBounds(text);
        var raw = new Uint8Array(300);
        for (var n = 0; n < bounds.starts.length; n++) {
            var start = bounds.starts[n];
            var stop = bounds.ends[n];
            while (stop > start && (text[stop - 1] === 10 || text[stop - 1] === 13)) stop--;
            var kind = String.fromCharCode(text[start + 1]);
            var digits = stop - start - 2;
            var bad = text[start] !== 83 || !(kind in ADDRESS_BYTES) || digits < 2 || digits % 2 || digits / 2 > raw.length;
            var length = digits / 2;
            var total = 0;
            for (var i = 0; !bad && i < length; i++) {
                var high = hexValue(text[start + 2 + 2 * i]);
                var low = hexValue(text[start + 3 + 2 * i]);
                if (high < 0 || low < 0) bad = true;
                raw[i] = (high << 4) | low;
                total += raw[i];
            }
            if (bad || raw[0] !== length - 1 || (total & 0xFF) !== 0xFF) throw new PatchError("the update file is damaged (bad S-record)");
            if (kind === "1" || kind === "2" || kind === "3") {
                var size = ADDRESS_BYTES[kind];
                var address = 0;
                for (i = 0; i < size; i++) address = address * 256 + raw[1 + i];
                var payload = raw.subarray(1 + size, length - 1);
                if (address + payload.length > FLASH_LIMIT) throw new PatchError("the update file is damaged (data outside the image)");
                image.set(payload, address);
                end = Math.max(end, address + payload.length);
            }
        }
        return image.slice(0, end);
    }

    var HEX = "0123456789ABCDEF";

    /* The S2 records of a block, written straight into one buffer. */
    function encodeRecords(baseAddress, block) {
        var count = block.length / RECORD_BYTES;
        var lineLength = 2 + 2 * (1 + 3 + RECORD_BYTES + 1) + 2;
        var out = new Uint8Array(count * lineLength);
        var raw = new Uint8Array(1 + 3 + RECORD_BYTES);
        var at = 0;
        for (var n = 0; n < count; n++) {
            var address = baseAddress + n * RECORD_BYTES;
            raw[0] = 3 + RECORD_BYTES + 1;
            raw[1] = (address >>> 16) & 0xFF;
            raw[2] = (address >>> 8) & 0xFF;
            raw[3] = address & 0xFF;
            raw.set(block.subarray(n * RECORD_BYTES, (n + 1) * RECORD_BYTES), 4);
            var total = 0;
            out[at++] = 83;
            out[at++] = 50;
            for (var i = 0; i < raw.length; i++) {
                total += raw[i];
                out[at++] = HEX.charCodeAt(raw[i] >> 4);
                out[at++] = HEX.charCodeAt(raw[i] & 15);
            }
            var check = 0xFF - (total & 0xFF);
            out[at++] = HEX.charCodeAt(check >> 4);
            out[at++] = HEX.charCodeAt(check & 15);
            out[at++] = 13;
            out[at++] = 10;
        }
        return out;
    }

    /* ---- The firmware's LZSS packing (4,096-byte window, matches of 3 to 18 bytes) ---- */

    function lzssDecode(source) {
        var ring = new Uint8Array(WINDOW);
        ring.fill(32, 0, WINDOW - MAX_MATCH);
        var cursor = WINDOW - MAX_MATCH;
        var output = new Uint8Array(Math.max(16, source.length * 3));
        var length = 0;
        var position = 0;
        var size = source.length;
        function put(value) {
            if (length === output.length) {
                var bigger = new Uint8Array(output.length * 2);
                bigger.set(output);
                output = bigger;
            }
            output[length++] = value;
            ring[cursor] = value;
            cursor = (cursor + 1) & (WINDOW - 1);
        }
        while (position < size) {
            var flags = source[position++];
            for (var bit = 0; bit < 8; bit++) {
                if (position >= size) break;
                if (flags & (1 << bit)) {
                    put(source[position++]);
                } else {
                    if (position + 2 > size) throw new PatchError("the packed application is damaged");
                    var low = source[position];
                    var high = source[position + 1];
                    position += 2;
                    var start = low | ((high & 0xF0) << 4);
                    var count = (high & 0x0F) + 3;
                    for (var i = 0; i < count; i++) put(ring[(start + i) & (WINDOW - 1)]);
                }
            }
        }
        return output.slice(0, length);
    }

    /*
     * Pack data as the firmware expects it, making the same choices as the
     * encoder the tested build was made with, so the result is the same
     * bytes: the longest match (3 to 18 bytes) starting in the last 4,096
     * positions, the most recent one on a tie, and a single byte instead
     * when the next position has a longer match. The positions are found
     * through chains of earlier positions with the same first three bytes.
     */
    function lzssEncode(data, progress) {
        var size = data.length;
        var output = new Uint8Array(size + Math.ceil(size / 8) + 16);
        var length = 0;
        var head = new Int32Array(1 << 16);
        head.fill(-1);
        var previous = new Int32Array(WINDOW);
        var inserted = 0;
        var position = 0;
        var nextReport = 0;
        var foundWhere = 0;

        function hash(at) {
            return Math.imul((data[at] << 16) | (data[at + 1] << 8) | data[at + 2], 2654435761) >>> 16;
        }

        function insertUpTo(newest) {
            for (; inserted <= newest; inserted++) {
                if (inserted + 2 >= size) continue;
                var key = hash(inserted);
                previous[inserted & (WINDOW - 1)] = head[key];
                head[key] = inserted;
            }
        }

        /* The longest match at `at` starting at or before `newest`, and (in foundWhere) the most recent such start. */
        function find(at, newest) {
            var limit = Math.min(size - at, MAX_MATCH);
            if (limit < 3) return 0;
            var oldest = Math.max(at - WINDOW, 0);
            var best = 0;
            var where = 0;
            var candidate = head[hash(at)];
            while (candidate >= oldest) {
                if (candidate <= newest && data[candidate + best] === data[at + best]) {
                    var match = 0;
                    while (match < limit && data[candidate + match] === data[at + match]) match++;
                    if (match > best) {
                        best = match;
                        where = candidate;
                        if (match === limit) break;
                    }
                }
                candidate = previous[candidate & (WINDOW - 1)];
            }
            if (best < 3) return 0;
            foundWhere = where;
            return best;
        }

        function step(until) {
            while (position < size && position < until) {
                var flagsAt = length++;
                var flags = 0;
                for (var bit = 0; bit < 8; bit++) {
                    if (position >= size) break;
                    insertUpTo(position - 1);
                    var best = find(position, position - 1);
                    var where = foundWhere;
                    if (best >= 3 && best < MAX_MATCH && size - position > 1 && find(position + 1, position - 1) > best) best = 0;
                    if (best >= 3) {
                        var ring = (WINDOW - MAX_MATCH + where) & (WINDOW - 1);
                        output[length++] = ring & 0xFF;
                        output[length++] = ((ring >> 4) & 0xF0) | (best - 3);
                        position += best;
                    } else {
                        flags |= 1 << bit;
                        output[length++] = data[position];
                        position += 1;
                    }
                }
                output[flagsAt] = flags;
            }
        }

        return (async function () {
            while (position < size) {
                if (progress && position >= nextReport) {
                    progress(position, size);
                    nextReport = position + Math.floor(size / 100) + 1;
                }
                step(position + 65536);
                await tick();
            }
            if (progress) progress(size, size);
            return output.slice(0, length);
        })();
    }

    /* ---- Building the two files ---- */

    var OFFICIAL = "the official XDJ-700 firmware version 1.15 UPD file";

    /* Why a file given as the official one is refused, saying what it is when that can be told; null if it is the official file. */
    async function refusal(file, data) {
        var hash = await sha256(file);
        if (hash === data.stock_sha256) return null;
        if (hash === data.mod_sha256) {
            return "this is already the mod (" + data.build + "). nothing to build: copy it as XDJ700.UPD to the top folder of a FAT32 USB stick. to build it again, with the restore file, provide " + OFFICIAL;
        }
        if (hash === data.restore_sha256) {
            return "this is the restore file this patcher makes: it puts the official firmware 1.15 back and cannot be patched. please provide " + OFFICIAL + ", unchanged";
        }
        var start = String.fromCharCode.apply(null, file.subarray(0, 160));
        var header = /\r\n([ -~]{1,16}?) *MAINVer([0-9]{1,2})\.([0-9]{1,3})\0/.exec(start);
        if (header) {
            var player = header[1].trim();
            var major = parseInt(header[2], 10), minor = parseInt(header[3], 10);
            var version = major + "." + header[3];
            if (player !== "XDJ-700") return "this is the update file of another player (" + player + "). please provide " + OFFICIAL;
            if (version === data.mod_tag) {
                return "this says it is the mod (version " + version + "), but it is not byte for byte the tested build (" + data.build + "): it is damaged, changed or another build. DO NOT flash it. to build it again, provide " + OFFICIAL;
            }
            if (major > 1 || (major === 1 && minor > 15)) {
                return "this XDJ-700 file says version " + version + ": it has already been modified (a mod or a restore file), so it cannot be patched. please provide " + OFFICIAL + ", unchanged";
            }
            if (major < 1 || minor < 15) return "this is an older XDJ-700 firmware (version " + version + "). please provide " + OFFICIAL;
            return "this file says XDJ-700 version 1.15, but it is not the official file unchanged: it has been modified or damaged. please provide " + OFFICIAL + ", unchanged";
        }
        return "this is not " + OFFICIAL + ". please provide that, unchanged. any other file is refused";
    }

    async function checkOfficial(stock, data) {
        var reason = await refusal(stock, data);
        if (reason) throw new PatchError(reason);
    }

    /* The MAIN component's 32-byte header with another version tag. */
    function retag(component, tag) {
        var header = component.slice(0, 32);
        var replacement = ascii("MAINVer" + tag + "\0");
        if (replacement.length !== STOCK_TAG.length) throw new PatchError("the version tag was not found");
        var found = false;
        for (var at = 0; at + STOCK_TAG.length <= header.length; at++) {
            if (equal(header.subarray(at, at + STOCK_TAG.length), STOCK_TAG)) {
                header.set(replacement, at);
                at += STOCK_TAG.length - 1;
                found = true;
            }
        }
        if (!found || equal(header, component.subarray(0, 32))) throw new PatchError("the version tag was not found");
        return header;
    }

    function withChecksum(body) {
        var crc = crcHqx(body);
        return concat([body, new Uint8Array([crc & 0xFF, crc >> 8])]);
    }

    function container(main, panel) {
        return concat([ascii(main.length + "\r\n" + panel.length + "\r\n"), main, panel]);
    }

    /* Bytes of flash the packed application takes: the stock length, or past it to a whole record (as the build does). */
    function applicationSpan(blockBytes) {
        return Math.max(APPLICATION_SPAN_BYTES, Math.ceil(blockBytes / RECORD_BYTES) * RECORD_BYTES);
    }

    /* Stock 1.15 under a higher version tag, so the player accepts it over the mod. */
    function assembleRestore(stock, data) {
        return checkOfficial(stock, data).then(function () {
            var parts = splitContainer(stock);
            var main = stock.subarray(parts.offset, parts.offset + parts.lengths[0]);
            var newMain = withChecksum(concat([retag(main, data.restore_tag), main.subarray(32, main.length - 2)]));
            return concat([stock.subarray(0, parts.offset), newMain, stock.subarray(parts.offset + parts.lengths[0])]);
        });
    }

    async function buildRestore(stock, data) {
        var restore = await assembleRestore(stock, data);
        if (await sha256(restore) !== data.restore_sha256) throw new PatchError("internal error: the restore file is not the expected one");
        return restore;
    }

    /* The patched update file, made from the official one (buildMod checks it is the tested build). */
    async function assembleMod(stock, data, progress) {
        await checkOfficial(stock, data);
        var parts = splitContainer(stock);
        var main = stock.subarray(parts.offset, parts.offset + parts.lengths[0]);
        var panel = stock.subarray(parts.offset + parts.lengths[0]);
        var records = main.subarray(32, main.length - 2);
        var bounds = lineBounds(records);
        if (bounds.starts.length <= BOOT_REGION_LINES) throw new PatchError("the update file is damaged (too short)");
        var image = decodeRecords(records);

        var packedBytes = u32(image, APPLICATION_OFFSET);
        var packedEnd = APPLICATION_OFFSET + 4 + packedBytes;
        if (packedEnd + 2 > image.length || u16(image, packedEnd) !== sum16(image, APPLICATION_OFFSET, packedEnd)) {
            throw new PatchError("the packed application's checksum is wrong");
        }
        var application = lzssDecode(image.subarray(APPLICATION_OFFSET + 4, packedEnd));
        if (await sha256(application) !== data.stock_application_sha256) throw new PatchError("the unpacked application is not the expected one");

        // Our bytes at the places the mod hooks in, and our code after the application's end.
        var tail = await inflate(fromBase64(data.tail), "deflate");
        var patched = new Uint8Array(application.length + tail.length);
        patched.set(application);
        data.changes.forEach(function (change) {
            var hex = change[1];
            for (var i = 0; i < hex.length / 2; i++) patched[change[0] + i] = parseInt(hex.substr(2 * i, 2), 16);
        });
        patched.set(tail, application.length);
        if (await sha256(patched) !== data.mod_application_sha256) throw new PatchError("internal error: the patched application is not the expected one");

        var packed = await lzssEncode(patched, progress);
        if (!equal(lzssDecode(packed), patched)) throw new PatchError("internal error: the packed application does not unpack to what went in");
        var blockLength = 4 + packed.length + 2;
        var span = applicationSpan(blockLength);
        if (APPLICATION_OFFSET + span > FLASH_LIMIT) throw new PatchError("internal error: the packed application does not fit");
        var block = new Uint8Array(span);
        block.fill(0xFF);
        new DataView(block.buffer).setUint32(0, packed.length, true);
        block.set(packed, 4);
        var check = sum16(block, 0, 4 + packed.length);
        block[4 + packed.length] = check & 0xFF;
        block[5 + packed.length] = check >> 8;

        var newMain = withChecksum(concat([
            retag(main, data.mod_tag),
            records.subarray(0, bounds.ends[BOOT_REGION_LINES - 1]),
            encodeRecords(APPLICATION_OFFSET, block),
            records.subarray(bounds.starts[bounds.starts.length - 1])
        ]));
        var mod = container(newMain, panel);

        // The two rules every build keeps: the boot region and the panel part are Pioneer's, untouched.
        if (!equal(decodeRecords(newMain.subarray(32, newMain.length - 2)).subarray(0, APPLICATION_OFFSET), image.subarray(0, APPLICATION_OFFSET))) {
            throw new PatchError("internal error: the boot region changed");
        }
        if (!equal(mod.subarray(mod.length - panel.length), panel)) throw new PatchError("internal error: the panel part changed");
        return mod;
    }

    /* The patched update file, byte for byte the build that was tested on the player. */
    async function buildMod(stock, data, progress) {
        var mod = await assembleMod(stock, data, progress);
        if (await sha256(mod) !== data.mod_sha256) throw new PatchError("internal error: the result is not the tested build");
        return mod;
    }

    /* What a file is: "mod", "restore", "official" (Pioneer's 1.15), "damaged" (says it is the mod but is not), "zip" or null. */
    async function identify(file, data) {
        if (isZip(file)) return "zip";
        var hash = await sha256(file);
        if (hash === data.mod_sha256) return "mod";
        if (hash === data.restore_sha256) return "restore";
        if (hash === data.stock_sha256) return "official";
        var start = String.fromCharCode.apply(null, file.subarray(0, 160));
        var header = /\r\n([ -~]{1,16}?) *MAINVer([0-9]{1,2})\.([0-9]{1,3})\0/.exec(start);
        if (header && header[1].trim() === "XDJ-700" && header[2] + "." + header[3] === data.mod_tag) return "damaged";
        return null;
    }

    /* What the XDJ700.UPD files in a zip are, as a list of identify() answers. */
    async function zipInside(file, data) {
        var kinds = [];
        try {
            var entries = zipEntries(file).filter(function (entry) {
                return entry.name.toUpperCase().slice(-10) === "XDJ700.UPD";
            });
            for (var i = 0; i < entries.length; i++) kinds.push(await identify(await zipRead(file, entries[i]), data));
        } catch (error) {
            return [];
        }
        return kinds;
    }

    var AGAIN = "copy XDJ700.UPD from the zip or folder made here to the top folder of your USB stick again, eject the stick, plug it back in and try again.";

    /* The check of the file on the stick: {text, ok}, ok when it is safe to flash. The Python patcher gives the same answers. */
    async function stickCheck(file, data, name) {
        var kind = await identify(file, data);
        var wrongName = name.toUpperCase() === "XDJ700.UPD" ? "" :
            " but it is called " + name + " on the stick: rename it to XDJ700.UPD, or the player will not see it.";
        if (kind === "mod") return { text: "confirmed: this is the tested build (" + data.build + "). safe to flash." + wrongName, ok: !wrongName };
        if (kind === "restore") {
            return { text: "confirmed: this is the restore file (the official firmware 1.15). safe to flash to go back." + wrongName, ok: !wrongName };
        }
        if (kind === "official") {
            return { text: "this is the official firmware 1.15 as Pioneer ships it, not a file made here. to install the mod, copy 1_mod/XDJ700.UPD from the zip or folder made here; to go back to stock, copy 2_stock_restore/XDJ700.UPD.", ok: false };
        }
        if (kind === "damaged") {
            return { text: "this says it is the mod (version " + data.mod_tag + "), but it is not byte for byte the tested build (" + data.build + "): it is damaged, changed or another build. DO NOT flash it. " + AGAIN, ok: false };
        }
        if (kind === "zip") {
            var inside = await zipInside(file, data);
            if (inside.indexOf("mod") >= 0) {
                return { text: "this is the mod, still in its zip. the player cannot flash from a zip: open it, copy the mod's XDJ700.UPD to the top folder of your USB stick, then drop that file here.", ok: false };
            }
            if (inside.indexOf("restore") >= 0) {
                return { text: "this is the restore file, still in its zip. the player cannot flash from a zip: open it, copy the XDJ700.UPD inside to the top folder of your USB stick, then drop that file here.", ok: false };
            }
            if (inside.indexOf("official") >= 0) {
                return { text: "this is Pioneer's zip with the official firmware 1.15, not a file made here. to install the mod, copy 1_mod/XDJ700.UPD from the zip or folder made here.", ok: false };
            }
        }
        return { text: "this file is not right. DO NOT flash it. " + AGAIN, ok: false };
    }

    root.XdjPatcher = {
        PatchError: PatchError,
        isOurs: isOurs,
        ISSUES: ISSUES,
        REPORT: REPORT,
        sha256: sha256,
        sha256Plain: sha256Plain,
        crcHqx: crcHqx,
        crc32: crc32,
        zipEntries: zipEntries,
        zipRead: zipRead,
        zipWrite: zipWrite,
        readOfficialFile: readOfficialFile,
        splitContainer: splitContainer,
        decodeRecords: decodeRecords,
        encodeRecords: encodeRecords,
        lzssDecode: lzssDecode,
        lzssEncode: lzssEncode,
        applicationSpan: applicationSpan,
        assembleMod: assembleMod,
        buildMod: buildMod,
        assembleRestore: assembleRestore,
        buildRestore: buildRestore,
        identify: identify,
        stickCheck: stickCheck,
        refusal: refusal,
        ascii: ascii
    };
    if (typeof module !== "undefined" && module.exports) module.exports = root.XdjPatcher;
})(typeof window !== "undefined" ? window : globalThis);
