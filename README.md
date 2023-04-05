Packs/unpacks JSON files to a TLV format with an external string storage.

The files have to be a series of non-comma-separated JSON objects.
Value types are restricted to `strings` `integers` and `booleans`.

Example file:
```json
{"key": "value", "another": 42}
{"other": false}
...
```
```
Usage: jpak file.json
    -h          print this message and exit
    -b file     packed binary to decode back to json
    -d file     string dictionary
    -o file     destination of decoded json
```

## Internals

The JSON lexer/parser is handwritten and uses `getc` isntead of file-mapping in case we might want to pipe input to the program instead of passing a file.

Input isn't manually buffered and instead I rely on the buffering done by the standard library to be "good enough".

Endianness isn't considered i.e. only safe to pack/unpack on the same machine.

Hashmap is also handwritten. The packer never removes keys from any map and hence this function is missing from the hasmap implementation. This allowed it to be a bit simpler - by omitting tombstones.

The TLV type field is 1 byte, length is 4 bytes and the string key is also 4 bytes. (Not very compact for lots booleans).

## Build

```bash
make
```
