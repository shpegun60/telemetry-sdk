#!/usr/bin/env python3
"""Execute an emitted ESM in Chromium against actual native wire fixtures.

Requires caller-installed Playwright. No physical device or external server.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import threading

from playwright.sync_api import sync_playwright


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    args = parser.parse_args()
    files = {"/device.js": (args.module.read_bytes(), "text/javascript")}
    for name in ("descriptor", "config", "request", "response", "scalars", "empty"):
        files["/" + name + ".bin"] = ((args.fixtures / (name + ".bin")).read_bytes(), "application/octet-stream")

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == "/":
                data, mime = b"<!doctype html><title>Generated ESM check</title>", "text/html"
            elif self.path in files:
                data, mime = files[self.path]
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *_):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(headless=True)
            try:
                page = browser.new_page()
                errors = []
                page.on("pageerror", lambda error: errors.append(str(error)))
                page.goto(f"http://127.0.0.1:{server.server_address[1]}/")
                result = page.evaluate("""async () => {
                    const api = await import('/device.js');
                    const load = async name => new Uint8Array(await (await fetch('/'+name+'.bin')).arrayBuffer());
                    let checks=0;
                    const check = value => { if (!value) throw new Error('browser check '+checks); ++checks; };
                    const desc=await load('descriptor');
                    check(api.matchesHeader(desc.subarray(0,64)));
                    for(let i=0;i<64;++i) { const h=desc.slice(0,64); h[i]^=1; check(!api.matchesHeader(h)); }
                    for (const [name,id] of [['config',api.device.Config.valueTypeId],
                        ['request',api.device.Inspect.requestTypeId],
                        ['response',api.device.Inspect.responseTypeId],
                        ['scalars',api.edge.Scalars.valueTypeId],['empty',api.edge.Good.responseTypeId]]) {
                        const bytes=await load(name), codec=api.codecs['Type_'+id], value=codec.decode(bytes);
                        const encoded=codec.encode(value);
                        check(encoded.length===bytes.length && encoded.every((x,i)=>x===bytes[i]));
                        const offset=new Uint8Array(bytes.length+8).fill(0x5a);
                        codec.encodeInto(value,new DataView(offset.buffer,3,bytes.length));
                        check(offset.slice(3,3+bytes.length).every((x,i)=>x===bytes[i]));
                        check(offset[0]===0x5a && offset[offset.length-1]===0x5a);
                    }
                    const config=api.device.Config.read.decode(await load('config'));
                    check(config.serial===18446744073709551615n && config.offset===-9223372036854775808n);
                    check(Object.is(config.gain,-0) && Object.getPrototypeOf(config)===null);
                    check(api.edge.Scalars.read.decode(await load('scalars')).f64===Infinity);
                    const bad=await load('config');bad[28]=2;
                    let rejected=false;try {api.device.Config.read.decode(bad);}catch(e){rejected=e instanceof api.TelemetryCodecError;}
                    check(rejected);
                    return {checks,browser:navigator.userAgent};
                }""")
                if errors or result["checks"] < 84:
                    raise RuntimeError("browser errors or insufficient checks: " + repr(errors))
                print(json.dumps(result))
                print(f"codegen browser checks={result['checks']} failures=0")
            finally:
                browser.close()
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
