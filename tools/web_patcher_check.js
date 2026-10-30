#!/usr/bin/env node
/* Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
/*
 * Runs pieces of the web page's patcher (tools/web_patcher_core.js) in Node,
 * for tests/test_web_patcher.py to compare with the Python patcher.
 *
 *   node tools/web_patcher_check.js sha256 FILE
 *   node tools/web_patcher_check.js crc FILE
 *   node tools/web_patcher_check.js lzss-encode IN OUT      (prints the time it took)
 *   node tools/web_patcher_check.js lzss-decode IN OUT
 *   node tools/web_patcher_check.js records ADDRESS IN OUT
 *   node tools/web_patcher_check.js decode-records IN OUT
 *   node tools/web_patcher_check.js read-official IN OUT
 *   node tools/web_patcher_check.js zip-write OUT NAME FILE [NAME FILE ...]
 *   node tools/web_patcher_check.js build DATA.json STOCK_OR_ZIP MOD_OUT RESTORE_OUT
 *   node tools/web_patcher_check.js identify DATA.json FILE
 *   node tools/web_patcher_check.js refusal DATA.json FILE
 *   node tools/web_patcher_check.js stick-check DATA.json FILE NAME
 *
 * Errors of the patcher are printed as {"error": "..."} with exit code 3.
 */
"use strict";

const fs = require("fs");
const path = require("path");
const patcher = require(path.join(__dirname, "web_patcher_core.js"));

function read(file) {
    return new Uint8Array(fs.readFileSync(file));
}

async function main(args) {
    const command = args[0];
    if (command === "sha256") {
        const data = read(args[1]);
        return { subtle: await patcher.sha256(data), plain: patcher.sha256Plain(data) };
    }
    if (command === "crc") {
        const data = read(args[1]);
        return { hqx: patcher.crcHqx(data), crc32: patcher.crc32(data) };
    }
    if (command === "lzss-encode") {
        const started = Date.now();
        fs.writeFileSync(args[2], await patcher.lzssEncode(read(args[1])));
        return { milliseconds: Date.now() - started };
    }
    if (command === "lzss-decode") {
        fs.writeFileSync(args[2], patcher.lzssDecode(read(args[1])));
        return {};
    }
    if (command === "records") {
        fs.writeFileSync(args[3], patcher.encodeRecords(parseInt(args[1], 0), read(args[2])));
        return {};
    }
    if (command === "decode-records") {
        fs.writeFileSync(args[2], patcher.decodeRecords(read(args[1])));
        return {};
    }
    if (command === "read-official") {
        fs.writeFileSync(args[2], await patcher.readOfficialFile(read(args[1])));
        return {};
    }
    if (command === "zip-write") {
        const files = [];
        for (let i = 2; i < args.length; i += 2) files.push({ name: args[i], data: read(args[i + 1]) });
        fs.writeFileSync(args[1], patcher.zipWrite(files));
        return {};
    }
    if (command === "build") {
        const data = JSON.parse(fs.readFileSync(args[1], "utf8"));
        // As the page takes it: the update file, or the zip it comes in.
        const stock = await patcher.readOfficialFile(read(args[2]));
        const started = Date.now();
        fs.writeFileSync(args[3], await patcher.buildMod(stock, data));
        fs.writeFileSync(args[4], await patcher.buildRestore(stock, data));
        return { milliseconds: Date.now() - started };
    }
    if (command === "identify") {
        const data = JSON.parse(fs.readFileSync(args[1], "utf8"));
        return { kind: await patcher.identify(read(args[2]), data) };
    }
    if (command === "refusal") {
        const data = JSON.parse(fs.readFileSync(args[1], "utf8"));
        return { refusal: await patcher.refusal(read(args[2]), data) };
    }
    if (command === "stick-check") {
        const data = JSON.parse(fs.readFileSync(args[1], "utf8"));
        return await patcher.stickCheck(read(args[2]), data, args[3]);
    }
    throw new Error("unknown command: " + command);
}

main(process.argv.slice(2)).then(
    (result) => { process.stdout.write(JSON.stringify(result) + "\n"); },
    (error) => {
        if (error instanceof patcher.PatchError) {
            process.stdout.write(JSON.stringify({ error: error.message }) + "\n");
            process.exit(3);
        }
        process.stderr.write(String(error && error.stack || error) + "\n");
        process.exit(1);
    }
);
