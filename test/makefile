
BIN = ../bin/jpak

all: input.json
	$(BIN) input.json
	$(BIN) -b input.bj -d input.dict -o output.json
	jq -e -n --argfile a input.json --argfile b output.json '$$a == $$b'

input.json:
	./gen.rb >input.json

clean:
	rm -f *.json *.bj *.dict
