Packs/unpacks JSON files to a TLV format with an external string storage.

The files have to be a series of non-comma-separated JSON objects.
Value types are restricted to `strings`, `integers` and `booleans`.

Example file:
```json
{"key": "value", "another": 42}
{"and so on": true}
{"and so forth": true}
```

## Usage 

```
Usage: jpak file.json
    -h          print this message and exit
    -b file     packed binary to decode back to json
    -d file     string dictionary
    -o file     destination of decoded json
    -g          print debug info to stdout when unpacking
```
Example:

```
jpak your_file.json
```

This will produce a binary `your_file.bj` and a string dictionary `your_file.dict`.


Or you could pipe it:

```
cat your_file.json | jpak
```

And to decode - supply the generated files and an output destination.

```
jpak -b binary.bj -d dictionary.dic -o output.json
```

## Internals

The JSON lexer/parser is handwritten and uses `getc` instead of file-mapping in case we might want to pipe input to the program instead of passing a file.

Input isn't manually buffered and instead I rely on the buffering done by the standard library to be "good enough". Uses maximum of 1 `ungetc` which should be safe.

Endianness isn't considered i.e. only safe to pack/unpack on the same machine.

Hashmap is also handwritten. The packer never removes keys from any map and hence this function is missing from the hashmap implementation. This allowed it to be a bit simpler - by omitting tombstones.

The TLV type field is 1 byte, length is 4 bytes and the string key is also 4 bytes. (Not very compact for lots booleans). Length and key could be smaller, but limits to string sizes and amounts aren't clearly specified so they're left at 4.

Strings have a hard limit of `MAX_STR` when lexing. Therefore that's the limit of your JSON strings as well.

If something goes wrong it prints an error and immediately quits.

## Testing

In the `test` directory you'll find a `makefile` you can run to verify the correctness of the program. You'll need `ruby` to run a script that generates a random record of json objects and you'll need `jq` to compare the input and output json files for equality.

## Build

```bash
make
```
