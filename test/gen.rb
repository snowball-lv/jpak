#!/usr/bin/env ruby

require "json"

ALPHABET = ('a'..'z').to_a.push('"', " ")

def randkey
    return ALPHABET.sample(rand(0..10)).join("")
end

def randval
    vals = [true, false, rand(2000000) - 1000000, randkey()]
    return vals.sample
end

1000.times do
    obj = {}
    rand(10).times do
        obj[randkey()] = randval()
    end
    puts obj.to_json
end