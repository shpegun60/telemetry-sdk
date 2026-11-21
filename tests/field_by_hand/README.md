# Reading a Field without the SDK: the bytes

[`run.py`](run.py) holds [Reading a Field without the SDK](../../doc/user/ReadingAFieldByHand.md)
and the value-encoding example of [WireV3.md](../../doc/WireV3.md#value-encoding) to the
bytes a device sends.

[`main.cpp`](main.cpp) runs the [group_tree example's](../../examples/group_tree/README.md)
Model behind the real `tcp::Server` with the `EncodedProfile`, as a device and the
client's emulator do. Each request is built with the SDK's own builders
(`resource::protocol::client`, `protocol::encoded::write`) and fed to the server as a
client sends it. The program prints the request and the server's reply as
`BLOCK <name> ... END`:
- LIST of the files;
- READ of the descriptor, its envelope and its 64-byte header;
- the Field record of `adc/channel1/Gain`, the ID `1:0`;
- the session written into `/telemetry/session.bin`;
- the TEQ1 FieldRead and its reply, whose `f32` must equal the device's own value;
- the canonical bytes of `adc::Config{1000, Triggered, true}`.

`run.py` compares every block marked `<!-- bytes:NAME -->` in the two documents with
the printed bytes. Only the hex before `;` on each line counts. It fails when:
- a byte differs;
- a document marks a block the program does not print;
- the walkthrough lacks one of its ten blocks;
- the walkthrough does not state the descriptor's fingerprint.

```sh
python tests/field_by_hand/run.py --cxx g++ --build-dir /tmp/field-by-hand
python3 tests/field_by_hand/run.py --cxx clang++-18 --sanitize --build-dir /tmp/field-by-hand-san
```

The build directory must be outside the checkout or under its ignored `build/`. Logs
and `summary.json` (input hashes, the fingerprint, every matched block) stay there. No
socket, board or network is used.
